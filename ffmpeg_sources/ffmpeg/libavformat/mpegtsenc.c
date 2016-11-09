/*
 * MPEG2 transport stream (aka DVB) muxer
 * Copyright (c) 2003 Fabrice Bellard
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "libavutil/avassert.h"
#include "libavutil/bswap.h"
#include "libavutil/crc.h"
#include "libavutil/dict.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mathematics.h"
#include "libavutil/opt.h"

#include "libavcodec/internal.h"

#include "avformat.h"
#include "avio_internal.h"
#include "internal.h"
#include "mpegts.h"

#define PCR_TIME_BASE 27000000

/* write DVB SI sections */

/*********************************************/
/* mpegts section writer */

typedef struct MpegTSSection {
    int pid;
    int cc;
    void (*write_packet)(struct MpegTSSection *s, const uint8_t *packet);
    void *opaque;
} MpegTSSection;

typedef struct MpegTSService {
    MpegTSSection pmt; /* MPEG2 pmt table context */
    int sid;           /* service ID */
    char *name;
    char *provider_name;
    int pcr_pid;
    int pcr_packet_count;
    int pcr_packet_period;
    AVProgram *program;
} MpegTSService;

// service_type values as defined in ETSI 300 468
enum {
    MPEGTS_SERVICE_TYPE_DIGITAL_TV                   = 0x01,
    MPEGTS_SERVICE_TYPE_DIGITAL_RADIO                = 0x02,
    MPEGTS_SERVICE_TYPE_TELETEXT                     = 0x03,
    MPEGTS_SERVICE_TYPE_ADVANCED_CODEC_DIGITAL_RADIO = 0x0A,
    MPEGTS_SERVICE_TYPE_MPEG2_DIGITAL_HDTV           = 0x11,
    MPEGTS_SERVICE_TYPE_ADVANCED_CODEC_DIGITAL_SDTV  = 0x16,
    MPEGTS_SERVICE_TYPE_ADVANCED_CODEC_DIGITAL_HDTV  = 0x19,
    MPEGTS_SERVICE_TYPE_ONE_SEG                      = 0xC0
};
typedef struct MpegTSWrite {
    const AVClass *av_class;
    MpegTSSection pat; /* MPEG2 pat table */
    MpegTSSection sdt; /* MPEG2 sdt table context */
    MpegTSService **services;
    MpegTSSection tot; /* MPEG2 tot table context */
    MpegTSSection nit; /* MPEG2 nit table context*/
    MpegTSSection eit; /* MPEG2 eit table context*/
    int nit_packet_count;
    int nit_packet_period;
    int tot_packet_count;
    int tot_packet_period;
    int sdt_packet_count;
    int sdt_packet_period;
    int pat_packet_count;
    int pat_packet_period;
    int eit_packet_count;
    int eit_packet_period;

    int final_nb_services;
    int area_code;
    int guard_interval;
    int transmission_mode;
    int physical_channel;
    int virtual_channel;
    int transmission_profile;
    int nb_services;
    int onid;
    int tsid;
    int64_t first_pcr;
    int mux_rate; ///< set to 1 when VBR
    int pes_payload_size;

    int transport_stream_id;
    int original_network_id;
    int service_id;
    int service_type;

    int pmt_start_pid;
    int start_pid;
    int m2ts_mode;

    int reemit_pat_pmt; // backward compatibility

    int pcr_period;
#define MPEGTS_FLAG_REEMIT_PAT_PMT  0x01
#define MPEGTS_FLAG_AAC_LATM        0x02
#define MPEGTS_FLAG_PAT_PMT_AT_FRAMES           0x04
#define MPEGTS_FLAG_SYSTEM_B        0x08
    int flags;
    int copyts;
    int tables_version;
    double pat_period;
    double sdt_period;
    double nit_period;
    double tot_period;
    double eit_period;
    int64_t last_pat_ts;
    int64_t last_sdt_ts;
    int64_t last_nit_ts;
    int64_t last_tot_ts;
    int64_t last_eit_ts;

    int omit_video_pes_length;
} MpegTSWrite;

/* a PES packet header is generated every DEFAULT_PES_HEADER_FREQ packets */
#define DEFAULT_PES_HEADER_FREQ  16
#define DEFAULT_PES_PAYLOAD_SIZE ((DEFAULT_PES_HEADER_FREQ - 1) * 184 + 170)

/* The section length is 12 bits. The first 2 are set to 0, the remaining
 * 10 bits should not exceed 1021. */
#define SECTION_LENGTH 1020

/* NOTE: 4 bytes must be left at the end for the crc32 */
static void mpegts_write_section(MpegTSSection *s, uint8_t *buf, int len)
{
    unsigned int crc;
    unsigned char packet[TS_PACKET_SIZE];
    const unsigned char *buf_ptr;
    unsigned char *q;
    int first, b, len1, left;

    crc = av_bswap32(av_crc(av_crc_get_table(AV_CRC_32_IEEE),
                            -1, buf, len - 4));

    buf[len - 4] = (crc >> 24) & 0xff;
    buf[len - 3] = (crc >> 16) & 0xff;
    buf[len - 2] = (crc >>  8) & 0xff;
    buf[len - 1] =  crc        & 0xff;

    /* send each packet */
    buf_ptr = buf;
    while (len > 0) {
        first = buf == buf_ptr;
        q     = packet;
        *q++  = 0x47;
        b     = s->pid >> 8;
        if (first)
            b |= 0x40;
        *q++  = b;
        *q++  = s->pid;
        s->cc = s->cc + 1 & 0xf;
        *q++  = 0x10 | s->cc;
        if (first)
            *q++ = 0; /* 0 offset */
        len1 = TS_PACKET_SIZE - (q - packet);
        if (len1 > len)
            len1 = len;
        memcpy(q, buf_ptr, len1);
        q += len1;
        /* add known padding data */
        left = TS_PACKET_SIZE - (q - packet);
        if (left > 0)
            memset(q, 0xff, left);

        s->write_packet(s, packet);

        buf_ptr += len1;
        len     -= len1;
    }
}

static inline void put16(uint8_t **q_ptr, int val)
{
    uint8_t *q;
    q      = *q_ptr;
    *q++   = val >> 8;
    *q++   = val;
    *q_ptr = q;
}

static int mpegts_write_section1(MpegTSSection *s, int tid, int id,
                                 int version, int sec_num, int last_sec_num,
                                 uint8_t *buf, int len)
{
    uint8_t section[SECTION_LENGTH], *q;
    unsigned int tot_len;
    /* reserved_future_use field must be set to 1 for SDT */
    unsigned int flags = tid == SDT_TID ? 0xf000 : 0xb000;

    tot_len = 3 + 5 + len + 4;
    /* check if not too big */
    if (tot_len > SECTION_LENGTH)
        return AVERROR_INVALIDDATA;

    q    = section;
    *q++ = tid;
    put16(&q, flags | (len + 5 + 4)); /* 5 byte header + 4 byte CRC */
    put16(&q, id);
    *q++ = 0xc1 | (version << 1); /* current_next_indicator = 1 */
    *q++ = sec_num;
    *q++ = last_sec_num;
    memcpy(q, buf, len);

    mpegts_write_section(s, section, tot_len);
    return 0;
}

/*********************************************/
/* mpegts writer */

#define DEFAULT_PROVIDER_NAME   "FFmpeg"
#define DEFAULT_SERVICE_NAME    "Service01"
#define DEFAULT_NETWORK_NAME    "LaPSI TV - UFRGS"
#define DEFAULT_COUNTRY_CODE    "BRA"

#define DEFAULT_NID		0x0640	// 1600d

/* we retransmit the SI info at this rate */
#define SDT_RETRANS_TIME 500
#define NIT_RETRANS_TIME 50 //Arbitrary value, the brazilian standard requests the NIT to be send every 10 secs.
#define TOT_RETRANS_TIME 100 //Arbitrary value, the brazilian standard requests the TOT to be send every 10 secs.
#define PAT_RETRANS_TIME 100
#define PCR_RETRANS_TIME 20
#define EIT_RETRANS_TIME 500
// TODO Add here the new tables retransmission rate

typedef struct MpegTSWriteStream {
    struct MpegTSService *service;
    int pid; /* stream associated pid */
    int cc;
    int payload_size;
    int first_pts_check; ///< first pts check needed
    int prev_payload_key;
    int64_t payload_pts;
    int64_t payload_dts;
    int payload_flags;
    uint8_t *payload;
    AVFormatContext *amux;
    AVRational user_tb;

    /* For Opus */
    int opus_queued_samples;
    int opus_pending_trim_start;
} MpegTSWriteStream;

typedef enum {
	GI1_32,
	GI1_16,
	GI1_8,
	GI1_4
} guard_interval_t;

typedef enum {
	MODE1,
	MODE2,
	MODE3,
	UNDEFINED
} transmission_mode_t;

static void mpegts_write_pat(AVFormatContext *s)
{
    MpegTSWrite *ts = s->priv_data;
    MpegTSService *service;
    uint8_t data[SECTION_LENGTH], *q;
    int i;

    q = data;
    for (i = 0; i < ts->nb_services; i++) {
        service = ts->services[i];
        put16(&q, service->sid);
        put16(&q, 0xe000 | service->pmt.pid);
    }
    mpegts_write_section1(&ts->pat, PAT_TID, ts->tsid, ts->tables_version, 0, 0,
                          data, q - data);
}

static int mpegts_write_pmt(AVFormatContext *s, MpegTSService *service)
{
    MpegTSWrite *ts = s->priv_data;
    uint8_t data[SECTION_LENGTH], *q, *desc_length_ptr, *program_info_length_ptr, *parental_rating_length_ptr;
    int val, stream_type, i, err = 0;

    q = data;
    put16(&q, 0xe000 | service->pcr_pid);

    program_info_length_ptr = q;
    q += 2; /* patched after */

    // Parental Rating Descriptor
    *q++ = 0x55; //tag
    parental_rating_length_ptr = q;
    *q++; //length, filled later
    //putstr8(&q, DEFAULT_COUNTRY_CODE);
    //country code with 3 chars, default is BRA
    *q++ = 'B';
    *q++ = 'R';
    *q++ = 'A';

    *q++ = 0x01; // RSV 1b | SEX 1b | VIOLENCE 1b | DRUGS 1b | RATING 4b

    //Fill  descriptor length
    parental_rating_length_ptr[0] = q - parental_rating_length_ptr - 1;

    /* put program info here */

    val = 0xf000 | (q - program_info_length_ptr - 2);
    program_info_length_ptr[0] = val >> 8;
    program_info_length_ptr[1] = val;

    for (i = 0; i < s->nb_streams; i++) {
        AVStream *st = s->streams[i];
        MpegTSWriteStream *ts_st = st->priv_data;
        AVDictionaryEntry *lang = av_dict_get(st->metadata, "language", NULL, 0);

        if (s->nb_programs) {
            int k, found = 0;
            AVProgram *program = service->program;

            for (k = 0; k < program->nb_stream_indexes; k++)
                if (program->stream_index[k] == i) {
                    found = 1;
                    break;
                }

            if (!found)
                continue;
        }

        if (q - data > SECTION_LENGTH - 32) {
            err = 1;
            break;
        }
        
        if( ts_st->service->sid == service->sid ) {
        switch (st->codecpar->codec_id) {
        case AV_CODEC_ID_MPEG1VIDEO:
        case AV_CODEC_ID_MPEG2VIDEO:
            stream_type = STREAM_TYPE_VIDEO_MPEG2;
            break;
        case AV_CODEC_ID_MPEG4:
            stream_type = STREAM_TYPE_VIDEO_MPEG4;
            break;
        case AV_CODEC_ID_H264:
            stream_type = STREAM_TYPE_VIDEO_H264;
            break;
        case AV_CODEC_ID_HEVC:
            stream_type = STREAM_TYPE_VIDEO_HEVC;
            break;
        case AV_CODEC_ID_CAVS:
            stream_type = STREAM_TYPE_VIDEO_CAVS;
            break;
        case AV_CODEC_ID_DIRAC:
            stream_type = STREAM_TYPE_VIDEO_DIRAC;
            break;
        case AV_CODEC_ID_VC1:
            stream_type = STREAM_TYPE_VIDEO_VC1;
            break;
        case AV_CODEC_ID_MP2:
        case AV_CODEC_ID_MP3:
            stream_type = STREAM_TYPE_AUDIO_MPEG1;
            break;
        case AV_CODEC_ID_AAC:
            stream_type = (ts->flags & MPEGTS_FLAG_AAC_LATM)
                          ? STREAM_TYPE_AUDIO_AAC_LATM
                          : STREAM_TYPE_AUDIO_AAC;
            break;
        case AV_CODEC_ID_AAC_LATM:
            stream_type = STREAM_TYPE_AUDIO_AAC_LATM;
            break;
        case AV_CODEC_ID_AC3:
            stream_type = (ts->flags & MPEGTS_FLAG_SYSTEM_B)
                          ? STREAM_TYPE_PRIVATE_DATA
                          : STREAM_TYPE_AUDIO_AC3;
            break;
        case AV_CODEC_ID_EAC3:
            stream_type = (ts->flags & MPEGTS_FLAG_SYSTEM_B)
                          ? STREAM_TYPE_PRIVATE_DATA
                          : STREAM_TYPE_AUDIO_EAC3;
            break;
        case AV_CODEC_ID_DTS:
            stream_type = STREAM_TYPE_AUDIO_DTS;
            break;
        case AV_CODEC_ID_TRUEHD:
            stream_type = STREAM_TYPE_AUDIO_TRUEHD;
            break;
        case AV_CODEC_ID_OPUS:
            stream_type = STREAM_TYPE_PRIVATE_DATA;
            break;
        default:
            stream_type = STREAM_TYPE_PRIVATE_DATA;
            break;
        }
        *q++ = stream_type;
        put16(&q, 0xe000 | ts_st->pid);
        desc_length_ptr = q;
        q += 2; /* patched after */

        /* write optional descriptors here */
        switch (st->codecpar->codec_type) {
        case AVMEDIA_TYPE_AUDIO:
            if (st->codecpar->codec_id==AV_CODEC_ID_AC3 && (ts->flags & MPEGTS_FLAG_SYSTEM_B)) {
                *q++=0x6a; // AC3 descriptor see A038 DVB SI
                *q++=1; // 1 byte, all flags sets to 0
                *q++=0; // omit all fields...
            }
            if (st->codecpar->codec_id==AV_CODEC_ID_EAC3 && (ts->flags & MPEGTS_FLAG_SYSTEM_B)) {
                *q++=0x7a; // EAC3 descriptor see A038 DVB SI
                *q++=1; // 1 byte, all flags sets to 0
                *q++=0; // omit all fields...
            }
            if (st->codecpar->codec_id==AV_CODEC_ID_S302M) {
                *q++ = 0x05; /* MPEG-2 registration descriptor*/
                *q++ = 4;
                *q++ = 'B';
                *q++ = 'S';
                *q++ = 'S';
                *q++ = 'D';
            }
            if (st->codecpar->codec_id==AV_CODEC_ID_OPUS) {
                /* 6 bytes registration descriptor, 4 bytes Opus audio descriptor */
                if (q - data > SECTION_LENGTH - 6 - 4) {
                    err = 1;
                    break;
                }

                *q++ = 0x05; /* MPEG-2 registration descriptor*/
                *q++ = 4;
                *q++ = 'O';
                *q++ = 'p';
                *q++ = 'u';
                *q++ = 's';

                *q++ = 0x7f; /* DVB extension descriptor */
                *q++ = 2;
                *q++ = 0x80;

                if (st->codecpar->extradata && st->codecpar->extradata_size >= 19) {
                    if (st->codecpar->extradata[18] == 0 && st->codecpar->channels <= 2) {
                        /* RTP mapping family */
                        *q++ = st->codecpar->channels;
                    } else if (st->codecpar->extradata[18] == 1 && st->codecpar->channels <= 8 &&
                               st->codecpar->extradata_size >= 21 + st->codecpar->channels) {
                        static const uint8_t coupled_stream_counts[9] = {
                            1, 0, 1, 1, 2, 2, 2, 3, 3
                        };
                        static const uint8_t channel_map_a[8][8] = {
                            {0},
                            {0, 1},
                            {0, 2, 1},
                            {0, 1, 2, 3},
                            {0, 4, 1, 2, 3},
                            {0, 4, 1, 2, 3, 5},
                            {0, 4, 1, 2, 3, 5, 6},
                            {0, 6, 1, 2, 3, 4, 5, 7},
                        };
                        static const uint8_t channel_map_b[8][8] = {
                            {0},
                            {0, 1},
                            {0, 1, 2},
                            {0, 1, 2, 3},
                            {0, 1, 2, 3, 4},
                            {0, 1, 2, 3, 4, 5},
                            {0, 1, 2, 3, 4, 5, 6},
                            {0, 1, 2, 3, 4, 5, 6, 7},
                        };
                        /* Vorbis mapping family */

                        if (st->codecpar->extradata[19] == st->codecpar->channels - coupled_stream_counts[st->codecpar->channels] &&
                            st->codecpar->extradata[20] == coupled_stream_counts[st->codecpar->channels] &&
                            memcmp(&st->codecpar->extradata[21], channel_map_a[st->codecpar->channels-1], st->codecpar->channels) == 0) {
                            *q++ = st->codecpar->channels;
                        } else if (st->codecpar->channels >= 2 && st->codecpar->extradata[19] == st->codecpar->channels &&
                                   st->codecpar->extradata[20] == 0 &&
                                   memcmp(&st->codecpar->extradata[21], channel_map_b[st->codecpar->channels-1], st->codecpar->channels) == 0) {
                            *q++ = st->codecpar->channels | 0x80;
                        } else {
                            /* Unsupported, could write an extended descriptor here */
                            av_log(s, AV_LOG_ERROR, "Unsupported Opus Vorbis-style channel mapping");
                            *q++ = 0xff;
                        }
                    } else {
                        /* Unsupported */
                        av_log(s, AV_LOG_ERROR, "Unsupported Opus channel mapping for family %d", st->codecpar->extradata[18]);
                        *q++ = 0xff;
                    }
                } else if (st->codecpar->channels <= 2) {
                    /* Assume RTP mapping family */
                    *q++ = st->codecpar->channels;
                } else {
                    /* Unsupported */
                    av_log(s, AV_LOG_ERROR, "Unsupported Opus channel mapping");
                    *q++ = 0xff;
                }
            }

            if (lang) {
                char *p;
                char *next = lang->value;
                uint8_t *len_ptr;

                *q++     = 0x0a; /* ISO 639 language descriptor */
                len_ptr  = q++;
                *len_ptr = 0;

                for (p = lang->value; next && *len_ptr < 255 / 4 * 4; p = next + 1) {
                    if (q - data > SECTION_LENGTH - 4) {
                        err = 1;
                        break;
                    }
                    next = strchr(p, ',');
                    if (strlen(p) != 3 && (!next || next != p + 3))
                        continue; /* not a 3-letter code */

                    *q++ = *p++;
                    *q++ = *p++;
                    *q++ = *p++;

                    if (st->disposition & AV_DISPOSITION_CLEAN_EFFECTS)
                        *q++ = 0x01;
                    else if (st->disposition & AV_DISPOSITION_HEARING_IMPAIRED)
                        *q++ = 0x02;
                    else if (st->disposition & AV_DISPOSITION_VISUAL_IMPAIRED)
                        *q++ = 0x03;
                    else
                        *q++ = 0; /* undefined type */

                    *len_ptr += 4;
                }

                if (*len_ptr == 0)
                    q -= 2; /* no language codes were written */
            }
            break;
        case AVMEDIA_TYPE_SUBTITLE:
        {
           const char default_language[] = "und";
           const char *language = lang && strlen(lang->value) >= 3 ? lang->value : default_language;

           if (st->codecpar->codec_id == AV_CODEC_ID_DVB_SUBTITLE) {
               uint8_t *len_ptr;
               int extradata_copied = 0;

               *q++ = 0x59; /* subtitling_descriptor */
               len_ptr = q++;

               while (strlen(language) >= 3) {
                   if (sizeof(data) - (q - data) < 8) { /* 8 bytes per DVB subtitle substream data */
                       err = 1;
                       break;
                   }
                   *q++ = *language++;
                   *q++ = *language++;
                   *q++ = *language++;
                   /* Skip comma */
                   if (*language != '\0')
                       language++;

                   if (st->codecpar->extradata_size - extradata_copied >= 5) {
                       *q++ = st->codecpar->extradata[extradata_copied + 4]; /* subtitling_type */
                       memcpy(q, st->codecpar->extradata + extradata_copied, 4); /* composition_page_id and ancillary_page_id */
                       extradata_copied += 5;
                       q += 4;
                   } else {
                       /* subtitling_type:
                        * 0x10 - normal with no monitor aspect ratio criticality
                        * 0x20 - for the hard of hearing with no monitor aspect ratio criticality */
                       *q++ = (st->disposition & AV_DISPOSITION_HEARING_IMPAIRED) ? 0x20 : 0x10;
                       if ((st->codecpar->extradata_size == 4) && (extradata_copied == 0)) {
                           /* support of old 4-byte extradata format */
                           memcpy(q, st->codecpar->extradata, 4); /* composition_page_id and ancillary_page_id */
                           extradata_copied += 4;
                           q += 4;
                       } else {
                           put16(&q, 1); /* composition_page_id */
                           put16(&q, 1); /* ancillary_page_id */
                       }
                   }
               }

               *len_ptr = q - len_ptr - 1;
           } else if (st->codecpar->codec_id == AV_CODEC_ID_DVB_TELETEXT) {
               uint8_t *len_ptr = NULL;
               int extradata_copied = 0;

               /* The descriptor tag. teletext_descriptor */
               *q++ = 0x56;
               len_ptr = q++;

               while (strlen(language) >= 3 && q - data < sizeof(data) - 6) {
                   *q++ = *language++;
                   *q++ = *language++;
                   *q++ = *language++;
                   /* Skip comma */
                   if (*language != '\0')
                       language++;

                   if (st->codecpar->extradata_size - 1 > extradata_copied) {
                       memcpy(q, st->codecpar->extradata + extradata_copied, 2);
                       extradata_copied += 2;
                       q += 2;
                   } else {
                       /* The Teletext descriptor:
                        * teletext_type: This 5-bit field indicates the type of Teletext page indicated. (0x01 Initial Teletext page)
                        * teletext_magazine_number: This is a 3-bit field which identifies the magazine number.
                        * teletext_page_number: This is an 8-bit field giving two 4-bit hex digits identifying the page number. */
                       *q++ = 0x08;
                       *q++ = 0x00;
                   }
               }

               *len_ptr = q - len_ptr - 1;
            }
        }
        break;
        case AVMEDIA_TYPE_VIDEO:
            if (stream_type == STREAM_TYPE_VIDEO_DIRAC) {
                *q++ = 0x05; /*MPEG-2 registration descriptor*/
                *q++ = 4;
                *q++ = 'd';
                *q++ = 'r';
                *q++ = 'a';
                *q++ = 'c';
            } else if (stream_type == STREAM_TYPE_VIDEO_VC1) {
                *q++ = 0x05; /*MPEG-2 registration descriptor*/
                *q++ = 4;
                *q++ = 'V';
                *q++ = 'C';
                *q++ = '-';
                *q++ = '1';
            }
            break;
        case AVMEDIA_TYPE_DATA:
            if (st->codecpar->codec_id == AV_CODEC_ID_SMPTE_KLV) {
                *q++ = 0x05; /* MPEG-2 registration descriptor */
                *q++ = 4;
                *q++ = 'K';
                *q++ = 'L';
                *q++ = 'V';
                *q++ = 'A';
            }
            break;
        }

        val = 0xf000 | (q - desc_length_ptr - 2);
        desc_length_ptr[0] = val >> 8;
        desc_length_ptr[1] = val;
    } //if stream service equal current service
    } //for all streams in the context
    if (err)
        av_log(s, AV_LOG_ERROR,
               "The PMT section cannot fit stream %d and all following streams.\n"
               "Try reducing the number of languages in the audio streams "
               "or the total number of streams.\n", i);

    mpegts_write_section1(&service->pmt, PMT_TID, service->sid, ts->tables_version, 0, 0,
                          data, q - data);
    return 0;
}

/* NOTE: !str is accepted for an empty string */
static void putstr8(uint8_t **q_ptr, const char *str)
{
    uint8_t *q;
    int len;

    q = *q_ptr;
    if (!str)
        len = 0;
    else
        len = strlen(str);
    *q++ = len;
    memcpy(q, str, len);
    q     += len;
    *q_ptr = q;
}

static void mpegts_write_sdt(AVFormatContext *s)
{
    MpegTSWrite *ts = s->priv_data;
    MpegTSService *service;
    uint8_t data[SECTION_LENGTH], *q, *desc_list_len_ptr, *desc_len_ptr;
    int i, running_status, free_ca_mode, val;

    q = data;
    put16(&q, ts->onid);
    *q++ = 0xff;
    for (i = 0; i < ts->nb_services; i++) {
        service = ts->services[i];
        put16(&q, service->sid);
        *q++              = 0xfc | 0x00; /* currently no EIT info */
        desc_list_len_ptr = q;
        q                += 2;
        running_status    = 4; /* running */
        free_ca_mode      = 0;

        /* write only one descriptor for the service name and provider */
        *q++         = 0x48;
        desc_len_ptr = q;
        q++;
        if( (service->sid & 0x18 >> 3 )) {//if true, is a 1-seg service
            *q++ = MPEGTS_SERVICE_TYPE_ONE_SEG; /* 1seg television service */
	    }
	    else {
            *q++ = MPEGTS_SERVICE_TYPE_DIGITAL_TV ; /* digital television service */
	    }
        putstr8(&q, service->provider_name);
        putstr8(&q, service->name);
        desc_len_ptr[0] = q - desc_len_ptr - 1;

        /* fill descriptor length */
        val = (running_status << 13) | (free_ca_mode << 12) |
              (q - desc_list_len_ptr - 2);
        desc_list_len_ptr[0] = val >> 8;
        desc_list_len_ptr[1] = val;
    }
    mpegts_write_section1(&ts->sdt, SDT_TID, ts->tsid, ts->tables_version, 0, 0,
                          data, q - data);
}

static void mpegts_write_nit(AVFormatContext *s)
{
	MpegTSWrite *ts = s->priv_data;
	uint8_t data[SECTION_LENGTH], *q, *desc_len_ptr, *ts_loop_len_ptr, *transp_desc_len_ptr;
	uint8_t *ts_info_desc_length_ptr, *service_list_desc_length_ptr, *part_rec_desc_length_ptr, *sys_mgmt_desc_length_ptr, *terr_del_sys_desc_length_ptr;
	int i, temp_val, ts_loop_length_val, transp_desc_len_val;

	q = data;
	
	desc_len_ptr = q;
        q += 2;

	//Network Name Descriptor
	*q++ = 0x40; //tag
        putstr8(&q, DEFAULT_NETWORK_NAME); //length and name string

	// System Management Descriptor
	*q++ = 0xFE; //tag
	sys_mgmt_desc_length_ptr = q;
	*q++; //length, filled later
	*q++ = 0x03; //Bcast flag '00' Open TV, Bcast ID: '000011'
	*q++ = 0x01; //Read from RBS1905.ts

	//Fill  descriptor length
	sys_mgmt_desc_length_ptr[0] = q - sys_mgmt_desc_length_ptr - 1;

	//Other Descriptors
	//...
	//...
	
	//Fill the descriptors length field
	temp_val = 0xF0 << 8 | (q - desc_len_ptr - 2);
	//av_log(s, AV_LOG_VERBOSE, "calculated length: %x %x %d \n", desc_len_ptr[0], desc_len_ptr[1], (q - desc_len_ptr - 2));
	desc_len_ptr[0] = temp_val >> 8;
	desc_len_ptr[1] = temp_val;

	//Begin of TS loop descriptors
	ts_loop_len_ptr = q;
	q +=2;

	//TS ID, 16bits
	put16(&q, ts->tsid);

	//Original Network ID, 16bits
	put16(&q, ts->onid);
	
	//Begin of transport descriptors
	transp_desc_len_ptr = q;
	q +=2;

	//First Descriptor
	//TS Information Descriptor
	*q++ = 0xCD; //tag
	ts_info_desc_length_ptr = q;
	*q++; //length, filled later
	*q++ = ts->virtual_channel; //remote control key id
	//av_log(s, AV_LOG_VERBOSE, "==== virtual channel : %d physical channel %d \n", ts->virtual_channel, ts->physical_channel);
	//length of ts name string, 6 bits | transmission type count, 2 bits
	*q++ = strlen(DEFAULT_NETWORK_NAME) << 2 | 0x2;
	memcpy(q, DEFAULT_NETWORK_NAME, strlen(DEFAULT_NETWORK_NAME));
	q += strlen(DEFAULT_NETWORK_NAME);

	switch (ts->transmission_profile) {
		case 1:
		default:
			for(i = 0; i < ts->nb_services; i++) {
			//	av_log(s, AV_LOG_VERBOSE, "==== service test fields: %x NW_ID:%x SVC_TYPE:%x PGM_NB:%x \n",
			//		ts->services[i]->sid,
			//		(( ts->services[i]->sid & 0xFFE0 ) >> 5 ),
			//		(( ts->services[i]->sid & 0x18 ) >> 3 ),
			//		(ts->services[i]->sid & 0x7 )
			//	);
				if( (ts->services[i]->sid & 0x18 >> 3 )) {//if true, is a 1-seg service
					*q++ = 0xAF; //transmission type: 0xAF: C
					*q++ = 0x01; //number of services of this transm. type
					put16(&q, ts->services[i]->sid);//service_ID
				}
				else {
					*q++ = 0x0F; //transmission type: 0x0F: A
					*q++ = 0x01; //number of services of this transm. type
					put16(&q, ts->services[i]->sid);//service_ID
				}
			}
		break;
		case 2:
			for(i = 0; i < ts->nb_services; i++) {
			//	av_log(s, AV_LOG_VERBOSE, "==== service test fields: %x NW_ID:%x SVC_TYPE:%x PGM_NB:%x \n",
			//		ts->services[i]->sid,
			//		(( ts->services[i]->sid & 0xFFE0 ) >> 5 ),
			//		(( ts->services[i]->sid & 0x18 ) >> 3 ),
			//		(ts->services[i]->sid & 0x7 )
			//	);
				if( (ts->services[i]->sid & 0x18 >> 3 )) {//if true, is a 1-seg service
					*q++ = 0xAF; //transmission type: 0xAF: C
					*q++ = 0x01; //number of services of this transm. type
					put16(&q, ts->services[i]->sid);//service_ID
				}
				else {
					*q++ = 0x0F; //transmission type: 0x0F: A
					*q++ = 0x01; //number of services of this transm. type
					put16(&q, ts->services[i]->sid);//service_ID
				}
			}
		break;
	}

	//Fill TS info descriptor length
	ts_info_desc_length_ptr[0] = q - ts_info_desc_length_ptr - 1;
	
	//Service List Descriptor
	*q++ = 0x41; //tag
	service_list_desc_length_ptr = q;
	*q++; //length, filled later

	for(i = 0; i < ts->nb_services; i++) {
		put16(&q, ts->services[i]->sid);//service_ID
		*q++ = 0x01; //service type 0x01 for Digital TV Service
	}

	//Fill Service list descriptor length
	service_list_desc_length_ptr[0] = q - service_list_desc_length_ptr - 1;

	for(i = 0; i < ts->nb_services; i++) {
		//av_log(s, AV_LOG_VERBOSE, "==== 1-seg Service test: %x \n", (ts->services[i]->sid & 0x18) >> 3 );
		if(((ts->services[i]->sid & 0x18) >> 3) == 0x3) {//if true, is a 1-seg service
			//av_log(s, AV_LOG_VERBOSE, "==== 1-seg Service detected, creating partial reception desriptor.\n" );
			// Partial Reception Descriptor
			*q++ = 0xFB; //tag
			part_rec_desc_length_ptr = q;
			*q++; //length, filled later
			put16(&q, ts->services[i]->sid);
			//Fill  descriptor length
			part_rec_desc_length_ptr[0] = q - part_rec_desc_length_ptr - 1;
		}
	}

	//// Terrestrial System Delivery Descriptor
	*q++ = 0xFA; //tag
	terr_del_sys_desc_length_ptr = q;
	*q++; //length, filled later
	put16(&q, ts->area_code << 4 | ts->guard_interval << 2 | ts->transmission_mode );// Area code | Guard interval | Transmission mode
	put16(&q,  ( 473 + 6 * ( ts->physical_channel - 14 ) +1/7 ) * 7 );// Frequency field: ( 473 + 6 * ( CH - 14 ) +1/7 ) * 7
	//*q++ = 0x; //

	////Fill  descriptor length
	terr_del_sys_desc_length_ptr[0] = q - terr_del_sys_desc_length_ptr - 1;


	//// Descriptor
	//*q++ = 0x41; //tag
	//_length_ptr = q;
	//*q++; //length, filled later
	//put16(&q, 0x);//
	//*q++ = 0x; //

	////Fill  descriptor length
	//_length_ptr[0] = q - _length_ptr - 1;



	//Fill the Transport descriptors length field first
	transp_desc_len_val = 0xF0 << 8 | (q - transp_desc_len_ptr - 2);

	transp_desc_len_ptr[0] = transp_desc_len_val >> 8;
	transp_desc_len_ptr[1] = transp_desc_len_val;


	//Fill the TS loop length field after, for it contains the Transp. descriptors
	ts_loop_length_val = 0xF0 << 8 | (q - ts_loop_len_ptr - 2);

	ts_loop_len_ptr[0] = ts_loop_length_val >> 8;
	ts_loop_len_ptr[1] = ts_loop_length_val;

	
	
	//Write the table
	mpegts_write_section1(&ts->nit, NIT_TID, ts->onid, ts->tables_version, 0, 0,
                          data, q - data);
}

static void mpegts_write_tot(AVFormatContext *s)
{
    MpegTSWrite *ts = s->priv_data;
    MpegTSService *service;
    uint8_t section[SECTION_LENGTH], *q, *tot_length_ptr, *desc_len_ptr, *offset_desc_length_ptr;
    int i, temp_val;
	unsigned int tot_length;

    q = section;
    *q++ = TOT_TID;
	tot_length_ptr = q;
	q += 2; //Filled later

    *q++ = 0xDD; //UTC-3 byte#0; year//TODO parametro de entrada
    *q++ = 0xE2; //UTC-3 byte#1; year//TODO parametro de entrada
    *q++ = 0x10; //UTC-3 byte#2; hour//TODO parametro de entrada
    *q++ = 0x20; //UTC-3 byte#3; min//TODO parametro de entrada
    *q++ = 0x30; //UTC-3 byte#4; sec//TODO parametro de entrada

	//Descriptors...	
	desc_len_ptr = q;
    q += 2;

	//Local Time Offset Descriptor
	*q++ = 0x58; //tag
	offset_desc_length_ptr = q;
	*q++; //length, filled later

	*q++ = 'B'; ////TODO parametro de entrada
	*q++ = 'R'; ////TODO parametro de entrada
	*q++ = 'A'; ////TODO parametro de entrada

	*q++ = 0x03 << 2 | 0x2; //Country Region ID, 6bits | RSV 1bit = '1' | POLARITY 1bit
	put16(&q, 0x0000);// Local Time Offset
	
	//Time of Change
    *q++ = 0xDE; //UTC-3 byte#0; year//TODO parametro de entrada
    *q++ = 0x7B; //UTC-3 byte#0; year//TODO parametro de entrada
    *q++ = 0x00; //UTC-3 byte#0; hour//TODO parametro de entrada
    *q++ = 0x00; //UTC-3 byte#0; min//TODO parametro de entrada
    *q++ = 0x00; //UTC-3 byte#0; sec//TODO parametro de entrada

	put16(&q, 0x0100);// Next Time Offset

	//Fill  descriptor length
	offset_desc_length_ptr[0] = q - offset_desc_length_ptr - 1;

	//Fill the descriptors length field
	temp_val = 0xF0 << 8 | (q - desc_len_ptr - 2);
	//av_log(s, AV_LOG_VERBOSE, "calculated length: %x %x %d \n", desc_len_ptr[0], desc_len_ptr[1], (q - desc_len_ptr - 2));
	desc_len_ptr[0] = temp_val >> 8;
	desc_len_ptr[1] = temp_val;



	//Section length field completion
	tot_length = q - tot_length_ptr - 2 + 4;// From beggining of UTC-3 field up to end of CRC: variable (q-ptr-2) + CRC (+4)
    put16(&tot_length_ptr, 0xB000 | tot_length); //number of bytes in the section after the two bytes of section_number

    mpegts_write_section(&ts->tot, section, tot_length + 3); // Add to tot_len the 1byte TID and the 2byte (flags | section_length)
}

static void mpegts_write_eit(AVFormatContext *s)
{
    MpegTSWrite *ts = s->priv_data;
    MpegTSService *service;
    uint8_t data[2500], *q, *desc_list_len_ptr, *short_event_desc_len, *event_name_len, *text_len;
    uint8_t *parental_rat_desc_len, *component_desc_len, *audio_comp_desc_len, *content_desc_len;
    int i, j, running_status, free_ca_mode, val;

    q = data;
    put16(&q, ts->tsid);
    put16(&q, ts->onid);
    *q++ = 0;//segment_last_section_number
    *q++ = EIT_TID;//last_table_id
    for(i = 0; i < ts->nb_services; i++){     //loop number of services
        service = ts->services[i];
        put16(&q, service->sid);
        //start_time
        *q++ = 0xDD; //UTC-3 byte#0; year //TODO parametro de entrada
        *q++ = 0xE2; //UTC-3 byte#1; year //TODO parametro de entrada
        *q++ = 0x10; //UTC-3 byte#2; hour //TODO parametro de entrada
        *q++ = 0x20; //UTC-3 byte#3; min  //TODO parametro de entrada
        *q++ = 0x30; //UTC-3 byte#4; sec; //TODO parametro de entrada
        //duration
        *q++ = 0x10; //UTC-3 byte#2; hour //TODO parametro de entrada
        *q++ = 0x20; //UTC-3 byte#3; min  //TODO parametro de entrada
        *q++ = 0x30; //UTC-3 byte#4; sec; //TODO parametro de entrada
        desc_list_len_ptr = q;
        q                += 2;
        //TODO defirnir running status como um if com os valores da TOT
        running_status    = 4; //running
        free_ca_mode      = 0;
        //insert descriptors bellow
        //TODO inserir descritores
        if( (service->sid & 0x18 >> 3 )) {//if true, is a 1-seg service
            //Short event descriptor
            *q++ = 0x4d;//descriptor tag
            short_event_desc_len = q;//descriptor length
            *q++; //length, filled later
            *q++ = 0x70;//ISO_639_language_code default language 'por' value 706F72
            *q++ = 0x6F;
            *q++ = 0x72;
            event_name_len = q;//event name length TODO coverter tudo isso para um loop for e parametros de entrada
            *q++;
            *q++ = 'L';
            *q++ = 'a';
            *q++ = 'P';
            *q++ = 'S';
            *q++ = 'I';
            event_name_len[0] = q - event_name_len -1;
            text_len = q;//text length TODO converter tudo isso para um loop for e parametros de entrada
            *q++;
            *q++ = 'N';
            text_len[0] = q - text_len -1;
            //Fill  descriptor length
            short_event_desc_len[0] = q - short_event_desc_len - 1;

            //Parental Rating descriptor
            *q++ = 0x55;
            parental_rat_desc_len = q;
            *q++ = 'B';//TODO converter para parametro de entrada
            *q++ = 'R';//TODO converter para parametro de entrada
            *q++ = 'A';//TODO converter para parametro de entrada
            *q++ = 1;  //TODO converter para parametro de entrada
            //Fill  descriptor length
            parental_rat_desc_len[0] = q - parental_rat_desc_len - 1;
        }
        else {
            //Short event descriptor
            *q++ = 0x4d;//descriptor tag
            short_event_desc_len = q;//descriptor length
            *q++; //length, filled later
            *q++ = 0x70;//ISO_639_language_code default language 'por' value 706F72
            *q++ = 0x6F;
            *q++ = 0x72;
            event_name_len = q;//event name length TODO coverter tudo isso para um loop for e parametros de entrada
            *q++;
            *q++ = 'L';
            *q++ = 'a';
            *q++ = 'P';
            *q++ = 'S';
            *q++ = 'I';
            event_name_len[0] = q - event_name_len -1;
            text_len = q;//text length TODO converter tudo isso para um loop for e parametros de entrada
            *q++;
            *q++ = 'N';
            text_len[0] = q - text_len -1;
            //Fill  descriptor length
            short_event_desc_len[0] = q - short_event_desc_len - 1;

            //Parental Rating descriptor
            *q++ = 0x55;
            parental_rat_desc_len = q;
            *q++ = 'B';//TODO converter para parametro de entrada
            *q++ = 'R';//TODO converter para parametro de entrada
            *q++ = 'A';//TODO converter para parametro de entrada
            *q++ = 1;  //TODO converter para parametro de entrada
            //Fill  descriptor length
            parental_rat_desc_len[0] = q - parental_rat_desc_len - 1;

            //Component Descriptor
            *q++ = 0x50;
            component_desc_len = q;
            *q++; //length filled later
            *q++ = 0x05;
            *q++ = 0xb3;
            *q++ = 0x00;
            *q++ = 0x70;//ISO_639_language_code default language 'por' value 706F72
            *q++ = 0x6F;
            *q++ = 0x72;
            *q++ = 'V'; //TODO converter para parametro de entrada e um loop
            *q++ = 'i';
            *q++ = 'd';
            *q++ = 'e';
            *q++ = 'o';
            //Fill descriptor length
            component_desc_len[0] = q - component_desc_len - 1;

            //Audio Component Descriptor TODO converter para parametro de entrada em um loop para cada audio
            *q++ = 0xc4;
            audio_comp_desc_len = q;
            *q++;//lentg filled later
            *q++ = 0x06; //stream_content
            *q++ = 0x03; //component_type
            *q++ = 0x10; //component_tag
            *q++ = 0x53; //ISO/IEC 14496-3 Audio
            *q++ = 0xff; //simulcast_group_tag
            *q++ =    0; //ES_multi_lingual_flag
            *q++ =    1; //main_component_flag
            *q++ =    1; //quality_indicator_mode
            *q++ = 0x2F; //qual_freq_res
            *q++ = 0x70;//ISO_639_language_code default language 'por' value 706F72
            *q++ = 0x6F;
            *q++ = 0x72;
            *q++ = 'A'; //TODO converter para parametro de entrada
            *q++ = 'u';
            *q++ = 'd';
            *q++ = 'i';
            *q++ = 'o';
            //Fill descriptor length
            audio_comp_desc_len[0] = q - audio_comp_desc_len - 1;

            //Content Descriptor
            *q++ = 0x54;
            content_desc_len = q;
            *q++;
            *q++ = 0x02; //content_nibble
            *q++ = 0xEE; //user_byte
            //Fill descriptor length
            content_desc_len[0] = q - content_desc_len - 1;


        }
        // end of descriptor field, now fill descriptor length
        val = (running_status << 13) | (free_ca_mode << 12) |
              (q - desc_list_len_ptr - 2);
        desc_list_len_ptr[0] = val >> 8;
        desc_list_len_ptr[1] = val;
    }
    mpegts_write_section1(&ts->eit, EIT_TID, service->sid, ts->tables_version, 0, 0,
                          data, q - data);
}


static MpegTSService *mpegts_add_service(MpegTSWrite *ts, int sid,
                                         const char *provider_name,
                                         const char *name)
{
    MpegTSService *service;

    service = av_mallocz(sizeof(MpegTSService));
    if (!service)
        return NULL;
    service->pmt.pid       = ts->pmt_start_pid + ts->nb_services;
    service->sid           = sid;
    service->pcr_pid       = 0x1fff;
    service->provider_name = av_strdup(provider_name);
    service->name          = av_strdup(name);
    if (!service->provider_name || !service->name)
        goto fail;
    if (av_dynarray_add_nofree(&ts->services, &ts->nb_services, service) < 0)
        goto fail;

    return service;
fail:
    av_freep(&service->provider_name);
    av_freep(&service->name);
    av_free(service);
    return NULL;
}

static int64_t get_pcr(const MpegTSWrite *ts, AVIOContext *pb)
{
    return av_rescale(avio_tell(pb) + 11, 8 * PCR_TIME_BASE, ts->mux_rate) +
           ts->first_pcr;
}

static void mpegts_prefix_m2ts_header(AVFormatContext *s)
{
    MpegTSWrite *ts = s->priv_data;
    if (ts->m2ts_mode) {
        int64_t pcr = get_pcr(s->priv_data, s->pb);
        uint32_t tp_extra_header = pcr % 0x3fffffff;
        tp_extra_header = AV_RB32(&tp_extra_header);
        avio_write(s->pb, (unsigned char *) &tp_extra_header,
                   sizeof(tp_extra_header));
    }
}

static void section_write_packet(MpegTSSection *s, const uint8_t *packet)
{
    AVFormatContext *ctx = s->opaque;
    mpegts_prefix_m2ts_header(ctx);
    avio_write(ctx->pb, packet, TS_PACKET_SIZE);
}

static int mpegts_init(AVFormatContext *s)
{
    MpegTSWrite *ts = s->priv_data;
    MpegTSWriteStream *ts_st;
    MpegTSService *service;
    AVStream *st, *pcr_st = NULL;
    AVDictionaryEntry *title, *provider;
    int i, j;
    const char *service_name;
    const char *provider_name;
    int *pids;
    int ret;
    int calculated_FHD_service_ID, calculated_1SEG_service_ID, calculated_SD_service_ID, calculated_HD_service_ID;

    if (s->max_delay < 0) /* Not set by the caller */
        s->max_delay = 0;


    ts->tsid = ts->transport_stream_id;
    ts->onid = ts->original_network_id;
    if (!s->nb_programs) {
        av_log(s, AV_LOG_ERROR, "\nNB_PROGRAMS =%d\n", s->nb_programs);

        /* allocate a single DVB service */
        title = av_dict_get(s->metadata, "service_name", NULL, 0);
        if (!title)
            title = av_dict_get(s->metadata, "title", NULL, 0);
        service_name  = title ? title->value : DEFAULT_SERVICE_NAME;
        provider      = av_dict_get(s->metadata, "service_provider", NULL, 0);
        provider_name = provider ? provider->value : DEFAULT_PROVIDER_NAME;
        switch (ts->transmission_profile) {

	    case 1://One FHD service and one 1SEG service
            av_log(s, AV_LOG_ERROR, "\nCase = 1.1\n");
		    //First we calculate the HD service ID based on the network_ID, service type (0x0 for TV, 0x3 for 1-SEG) and program counter
		    calculated_FHD_service_ID = 0x0000; //Initialization necessary?
		    calculated_FHD_service_ID = ( ts->onid & 0x7FF ) << 5 | 0x0 << 3 | 0x0;

	        service = mpegts_add_service(ts, calculated_FHD_service_ID, provider_name, "SVC HD Full Seg");
	        service->pmt.write_packet = section_write_packet;
	        service->pmt.opaque = s;
	        service->pmt.cc = 15;

		    calculated_1SEG_service_ID = 0x0000; //Initialization necessary?
		    calculated_1SEG_service_ID = ( ts->onid & 0x7FF ) << 5 | 0x3 << 3 | 0x1;

		    service = mpegts_add_service(ts, calculated_1SEG_service_ID, provider_name, "SVC LD 1-Seg");
		    service->pmt.write_packet = section_write_packet;
		    service->pmt.opaque = s;
		    service->pmt.cc = 15;

		    ts->final_nb_services = 2;
	        break;
	    case 2://Four SD services and one 1SEG service
            av_log(s, AV_LOG_ERROR, "\nCase = 2.1\n");
            calculated_SD_service_ID = 0x0000;
            calculated_SD_service_ID = ( ts->onid & 0x7FF ) << 5 | 0x0 << 3 | 0x0;

            service = mpegts_add_service(ts, calculated_SD_service_ID, provider_name, "SVC SD - 1");
	        service->pmt.write_packet = section_write_packet;
	        service->pmt.opaque = s;
	        service->pmt.cc = 15;
            
            calculated_SD_service_ID = 0x0000;
            calculated_SD_service_ID = ( ts->onid & 0x7FF ) << 5 | 0x0 << 3 | 0x1;

            service = mpegts_add_service(ts, calculated_SD_service_ID, provider_name, "SVC SD - 2");
	        service->pmt.write_packet = section_write_packet;
	        service->pmt.opaque = s;
	        service->pmt.cc = 15;

            calculated_SD_service_ID = 0x0000;
            calculated_SD_service_ID = ( ts->onid & 0x7FF ) << 5 | 0x0 << 3 | 0x2;

            service = mpegts_add_service(ts, calculated_SD_service_ID, provider_name, "SVC SD - 3");
	        service->pmt.write_packet = section_write_packet;
	        service->pmt.opaque = s;
	        service->pmt.cc = 15;

            calculated_SD_service_ID = 0x0000;
            calculated_SD_service_ID = ( ts->onid & 0x7FF ) << 5 | 0x0 << 3 | 0x3;

            service = mpegts_add_service(ts, calculated_SD_service_ID, provider_name, "SVC SD - 4");
	        service->pmt.write_packet = section_write_packet;
	        service->pmt.opaque = s;
	        service->pmt.cc = 15;

            calculated_1SEG_service_ID = 0x0000; //Initialization necessary?
		    calculated_1SEG_service_ID = ( ts->onid & 0x7FF ) << 5 | 0x3 << 3 | 0x4;

		    service = mpegts_add_service(ts, calculated_1SEG_service_ID, provider_name, "SVC LD 1-Seg");
		    service->pmt.write_packet = section_write_packet;
		    service->pmt.opaque = s;
		    service->pmt.cc = 15;

            ts->final_nb_services = 5;
 	        break;
        case 3: //Two HD services and one 1SEG service
            av_log(s, AV_LOG_ERROR, "\nCase = 3.1\n");
            calculated_HD_service_ID = 0x0000;
            calculated_HD_service_ID = ( ts->onid & 0x7FF ) << 5 | 0x0 << 3 | 0x0;

            service = mpegts_add_service(ts, calculated_HD_service_ID, provider_name, "SVC HD - 1");
	        service->pmt.write_packet = section_write_packet;
	        service->pmt.opaque = s;
	        service->pmt.cc = 15;
            
            calculated_HD_service_ID = 0x0000;
            calculated_HD_service_ID = ( ts->onid & 0x7FF ) << 5 | 0x0 << 3 | 0x1;

            service = mpegts_add_service(ts, calculated_HD_service_ID, provider_name, "SVC HD - 2");
	        service->pmt.write_packet = section_write_packet;
	        service->pmt.opaque = s;
	        service->pmt.cc = 15;

            calculated_1SEG_service_ID = 0x0000;
		    calculated_1SEG_service_ID = ( ts->onid & 0x7FF ) << 5 | 0x3 << 3 | 0x4;

		    service = mpegts_add_service(ts, calculated_1SEG_service_ID, provider_name, "SVC LD 1-Seg");
		    service->pmt.write_packet = section_write_packet;
		    service->pmt.opaque = s;
		    service->pmt.cc = 15;
            ts->final_nb_services = 3;
 	        break;
        default:
            av_log(s, AV_LOG_ERROR, "\nCase = default.1\n");
            service = mpegts_add_service(ts, ts->service_id, provider_name, service_name);
            service->pmt.write_packet = section_write_packet;
            service->pmt.opaque = s;
            service->pmt.cc = 15;
            ts->final_nb_services = 1;
            break;
        }
       
        //service       = mpegts_add_service(ts, ts->service_id,provider_name, service_name);

        if (!service)
            return AVERROR(ENOMEM);

        //service->pmt.write_packet = section_write_packet;
        //service->pmt.opaque       = s;
        //service->pmt.cc           = 15;
    } else {
        for (i = 0; i < s->nb_programs; i++) {
            AVProgram *program = s->programs[i];
            title = av_dict_get(program->metadata, "service_name", NULL, 0);
            if (!title)
                title = av_dict_get(program->metadata, "title", NULL, 0);
            service_name  = title ? title->value : DEFAULT_SERVICE_NAME;
            provider      = av_dict_get(program->metadata, "service_provider", NULL, 0);
            provider_name = provider ? provider->value : DEFAULT_PROVIDER_NAME;
            switch (ts->transmission_profile) {
                av_log(s, AV_LOG_ERROR, "\nCase = 1.2\n");

                case 1://One FHD service and one 1SEG service

                    //First we calculate the HD service ID based on the network_ID, service type (0x0 for TV, 0x3 for 1-SEG) and program counter
                    calculated_FHD_service_ID = 0x0000; //Initialization necessary?
                    calculated_FHD_service_ID = ( ts->onid & 0x7FF ) << 5 | 0x0 << 3 | 0x0;

                    service = mpegts_add_service(ts, calculated_FHD_service_ID, provider_name, "SVC HD Full Seg");
                    service->pmt.write_packet = section_write_packet;
                    service->pmt.opaque = s;
                    service->pmt.cc = 15;

                    calculated_1SEG_service_ID = 0x0000; //Initialization necessary?
                    calculated_1SEG_service_ID = ( ts->onid & 0x7FF ) << 5 | 0x3 << 3 | 0x1;

                    service = mpegts_add_service(ts, calculated_1SEG_service_ID, provider_name, "SVC LD 1-Seg");
                    service->pmt.write_packet = section_write_packet;
                    service->pmt.opaque = s;
                    service->pmt.cc = 15;

                    ts->final_nb_services = 2;
                    break;
                case 2://Four SD services and one 1SEG service
                    av_log(s, AV_LOG_ERROR, "\nCase = 2.2\n");
                    calculated_SD_service_ID = 0x0000;
                    calculated_SD_service_ID = ( ts->onid & 0x7FF ) << 5 | 0x0 << 3 | 0x0;

                    service = mpegts_add_service(ts, calculated_SD_service_ID, provider_name, "SVC SD - 1");
                    service->pmt.write_packet = section_write_packet;
                    service->pmt.opaque = s;
                    service->pmt.cc = 15;

                    calculated_SD_service_ID = 0x0000;
                    calculated_SD_service_ID = ( ts->onid & 0x7FF ) << 5 | 0x0 << 3 | 0x1;

                    service = mpegts_add_service(ts, calculated_SD_service_ID, provider_name, "SVC SD - 2");
                    service->pmt.write_packet = section_write_packet;
                    service->pmt.opaque = s;
                    service->pmt.cc = 15;

                    calculated_SD_service_ID = 0x0000;
                    calculated_SD_service_ID = ( ts->onid & 0x7FF ) << 5 | 0x0 << 3 | 0x2;

                    service = mpegts_add_service(ts, calculated_SD_service_ID, provider_name, "SVC SD - 3");
                    service->pmt.write_packet = section_write_packet;
                    service->pmt.opaque = s;
                    service->pmt.cc = 15;

                    calculated_SD_service_ID = 0x0000;
                    calculated_SD_service_ID = ( ts->onid & 0x7FF ) << 5 | 0x0 << 3 | 0x3;

                    service = mpegts_add_service(ts, calculated_SD_service_ID, provider_name, "SVC SD - 4");
                    service->pmt.write_packet = section_write_packet;
                    service->pmt.opaque = s;
                    service->pmt.cc = 15;

                    calculated_1SEG_service_ID = 0x0000; //Initialization necessary?
                    calculated_1SEG_service_ID = ( ts->onid & 0x7FF ) << 5 | 0x3 << 3 | 0x4;

                    service = mpegts_add_service(ts, calculated_1SEG_service_ID, provider_name, "SVC LD 1-Seg");
                    service->pmt.write_packet = section_write_packet;
                    service->pmt.opaque = s;
                    service->pmt.cc = 15;

                    ts->final_nb_services = 5;
                    break;
                case 3: //Two HD services and one 1SEG service
                    av_log(s, AV_LOG_ERROR, "\nCase = 3.2\n");
                    calculated_HD_service_ID = 0x0000;
                    calculated_HD_service_ID = ( ts->onid & 0x7FF ) << 5 | 0x0 << 3 | 0x0;

                    service = mpegts_add_service(ts, calculated_HD_service_ID, provider_name, "SVC HD - 1");
                    service->pmt.write_packet = section_write_packet;
                    service->pmt.opaque = s;
                    service->pmt.cc = 15;

                    calculated_HD_service_ID = 0x0000;
                    calculated_HD_service_ID = ( ts->onid & 0x7FF ) << 5 | 0x0 << 3 | 0x1;

                    service = mpegts_add_service(ts, calculated_HD_service_ID, provider_name, "SVC HD - 2");
                    service->pmt.write_packet = section_write_packet;
                    service->pmt.opaque = s;
                    service->pmt.cc = 15;

                    calculated_1SEG_service_ID = 0x0000;
                    calculated_1SEG_service_ID = ( ts->onid & 0x7FF ) << 5 | 0x3 << 3 | 0x4;

                    service = mpegts_add_service(ts, calculated_1SEG_service_ID, provider_name, "SVC LD 1-Seg");
                    service->pmt.write_packet = section_write_packet;
                    service->pmt.opaque = s;
                    service->pmt.cc = 15;
                    ts->final_nb_services = 3;
                    break;
                default:
                    av_log(s, AV_LOG_ERROR, "\nCase = default.2\n");
                    service = mpegts_add_service(ts, ts->service_id, provider_name, service_name);
                    service->pmt.write_packet = section_write_packet;
                    service->pmt.opaque = s;
                    service->pmt.cc = 15;
                    ts->final_nb_services = 1;
                    break;
            }
            //service       = mpegts_add_service(ts, program->id, provider_name, service_name);

            if (!service)
                return AVERROR(ENOMEM);

            //service->pmt.write_packet = section_write_packet;
            //service->pmt.opaque       = s;
            //service->pmt.cc           = 15;
            //service->program          = program;
        }
    }

    ts->pat.pid          = PAT_PID;
    /* Initialize at 15 so that it wraps and is equal to 0 for the
     * first packet we write. */
    ts->pat.cc           = 15;
    ts->pat.write_packet = section_write_packet;
    ts->pat.opaque       = s;

    ts->sdt.pid          = SDT_PID;
    ts->sdt.cc           = 15;
    ts->sdt.write_packet = section_write_packet;
    ts->sdt.opaque       = s;

    ts->nit.pid          = NIT_PID;
    ts->nit.cc           = 15;
    ts->nit.write_packet = section_write_packet;
    ts->nit.opaque       = s;

    ts->tot.pid          = TOT_PID;
    ts->tot.cc           = 15;
    ts->tot.write_packet = section_write_packet;
    ts->tot.opaque       = s;
    ts->eit.pid          = EIT_PID;
    ts->eit.cc           = 15;
    ts->eit.write_packet = section_write_packet;
    ts->eit.opaque       = s;
    pids = av_malloc_array(s->nb_streams, sizeof(*pids));
    if (!pids) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    /* assign pids to each stream */
    for (i = 0; i < s->nb_streams; i++) {
        AVProgram *program;
        st = s->streams[i];

        ts_st = av_mallocz(sizeof(MpegTSWriteStream));
        if (!ts_st) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        st->priv_data = ts_st;

        ts_st->user_tb = st->time_base;
        avpriv_set_pts_info(st, 33, 1, 90000);

        ts_st->payload = av_mallocz(ts->pes_payload_size);
        if (!ts_st->payload) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        program = av_find_program_from_stream(s, NULL, i);
        if (program) {
            for (j = 0; j < ts->nb_services; j++) {
                if (ts->services[j]->program == program) {
                    service = ts->services[j];
                    break;
                }
            }
        }

        ts_st->service = ts->services[i % ts->final_nb_services] ; //TODO Potential point to modify the stream's owners.
        /* MPEG pid values < 16 are reserved. Applications which set st->id in
         * this range are assigned a calculated pid. */
        if (st->id < 16) {
            ts_st->pid = ts->start_pid + i;
        } else if (st->id < 0x1FFF) {
            ts_st->pid = st->id;
        } else {
            av_log(s, AV_LOG_ERROR,
                   "Invalid stream id %d, must be less than 8191\n", st->id);
            ret = AVERROR(EINVAL);
            goto fail;
        }
        if (ts_st->pid == ts_st->service->pmt.pid) {
            av_log(s, AV_LOG_ERROR, "Duplicate stream id %d\n", ts_st->pid);
            ret = AVERROR(EINVAL);
            goto fail;
        }
        for (j = 0; j < i; j++) {
            if (pids[j] == ts_st->pid) {
                av_log(s, AV_LOG_ERROR, "Duplicate stream id %d\n", ts_st->pid);
                ret = AVERROR(EINVAL);
                goto fail;
            }
        }
        pids[i]                = ts_st->pid;
        ts_st->payload_pts     = AV_NOPTS_VALUE;
        ts_st->payload_dts     = AV_NOPTS_VALUE;
        ts_st->first_pts_check = 1;
        ts_st->cc              = 15;
        /* update PCR pid by using the first video stream */
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
            ts_st->service->pcr_pid == 0x1fff) {
            ts_st->service->pcr_pid = ts_st->pid;
            pcr_st           = st;
        }
        if (st->codecpar->codec_id == AV_CODEC_ID_AAC &&
            st->codecpar->extradata_size > 0) {
            AVStream *ast;
            ts_st->amux = avformat_alloc_context();
            if (!ts_st->amux) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }
            ts_st->amux->oformat =
                av_guess_format((ts->flags & MPEGTS_FLAG_AAC_LATM) ? "latm" : "adts",
                                NULL, NULL);
            if (!ts_st->amux->oformat) {
                ret = AVERROR(EINVAL);
                goto fail;
            }
            if (!(ast = avformat_new_stream(ts_st->amux, NULL))) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }
            ret = avcodec_parameters_copy(ast->codecpar, st->codecpar);
            if (ret != 0)
                goto fail;
            ast->time_base = st->time_base;
            ret = avformat_write_header(ts_st->amux, NULL);
            if (ret < 0)
                goto fail;
        }
        if (st->codecpar->codec_id == AV_CODEC_ID_OPUS) {
            ts_st->opus_pending_trim_start = st->codecpar->initial_padding * 48000 / st->codecpar->sample_rate;
        }
    }

    av_freep(&pids);

    /* if no video stream, use the first stream as PCR */
    if (ts_st->service->pcr_pid == 0x1fff && s->nb_streams > 0) {
        pcr_st           = s->streams[0];
        ts_st            = pcr_st->priv_data;
        ts_st->service->pcr_pid = ts_st->pid;
    } else
        ts_st = pcr_st->priv_data;
    av_log(s, AV_LOG_VERBOSE, "\nTS MUX RATE: %d\n", ts->mux_rate);
    if (ts->mux_rate > 1) {
        ts_st->service->pcr_packet_period = (ts->mux_rate * PCR_RETRANS_TIME) /
                                     (TS_PACKET_SIZE * 8 * 1000);
        ts->sdt_packet_period      = (int64_t)ts->mux_rate * SDT_RETRANS_TIME /
                                     (TS_PACKET_SIZE * 8 * 1000);
        ts->pat_packet_period      = (int64_t)ts->mux_rate * PAT_RETRANS_TIME /
                                     (TS_PACKET_SIZE * 8 * 1000);
        ts->nit_packet_period      = (int64_t)ts->mux_rate * NIT_RETRANS_TIME /
                                     (TS_PACKET_SIZE * 8 * 1000);
        ts->tot_packet_period      = (int64_t)ts->mux_rate * TOT_RETRANS_TIME /
                                     (TS_PACKET_SIZE * 8 * 1000);
        ts->eit_packet_period      = (int64_t)ts->mux_rate * EIT_RETRANS_TIME /
                                     (TS_PACKET_SIZE * 8 * 1000);

        if (ts->copyts < 1)
            ts->first_pcr = av_rescale(s->max_delay, PCR_TIME_BASE, AV_TIME_BASE);
    } else {
        /* Arbitrary values, PAT/PMT will also be written on video key frames */
        ts->sdt_packet_period = 200;
        ts->pat_packet_period = 40;
        ts->nit_packet_period = 200; /*Suspeito valor arbitrario, rever*/
        ts->tot_packet_period = 200; /*Suspeito valor arbitrario, rever*/
        ts->eit_packet_period = 200; /*Suspeito valor arbitrario, rever*/
        if (pcr_st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            int frame_size = av_get_audio_frame_duration2(pcr_st->codecpar, 0);
            if (!frame_size) {
                av_log(s, AV_LOG_WARNING, "frame size not set\n");
                ts_st->service->pcr_packet_period =
                    pcr_st->codecpar->sample_rate / (10 * 512);
            } else {
                ts_st->service->pcr_packet_period =
                    pcr_st->codecpar->sample_rate / (10 * frame_size);
            }
        } else {
            // max delta PCR 0.1s
            // TODO: should be avg_frame_rate
            ts_st->service->pcr_packet_period =
                ts_st->user_tb.den / (10 * ts_st->user_tb.num);
        }
        if (!ts_st->service->pcr_packet_period)
            ts_st->service->pcr_packet_period = 1;
    }
    ts->last_pat_ts = AV_NOPTS_VALUE;
    ts->last_sdt_ts = AV_NOPTS_VALUE;
    ts->last_nit_ts = AV_NOPTS_VALUE;
    ts->last_tot_ts = AV_NOPTS_VALUE;
    ts->last_eit_ts = AV_NOPTS_VALUE;
    // The user specified a period, use only it
    if (ts->pat_period < INT_MAX/2) {
        ts->pat_packet_period = INT_MAX;
    }
    if (ts->sdt_period < INT_MAX/2) {
        ts->sdt_packet_period = INT_MAX;
    }
    if (ts->nit_period < INT_MAX/2) {
        ts->nit_packet_period = INT_MAX;
    }
    if (ts->tot_period < INT_MAX/2) {
        ts->tot_packet_period = INT_MAX;
    }
    if (ts->eit_period < INT_MAX/2) {
        ts->eit_packet_period = INT_MAX;
    }

    // output a PCR as soon as possible
    ts_st->service->pcr_packet_count = ts_st->service->pcr_packet_period;
    ts->pat_packet_count      = ts->pat_packet_period - 1;
    ts->sdt_packet_count      = ts->sdt_packet_period - 1;
    ts->nit_packet_count      = ts->nit_packet_period - 1;
    ts->tot_packet_count      = ts->tot_packet_period - 1;
    ts->eit_packet_count      = ts->eit_packet_period - 1;

    if (ts->mux_rate == 1)
        av_log(s, AV_LOG_VERBOSE, "muxrate VBR, ");
    else
        av_log(s, AV_LOG_VERBOSE, "muxrate %d, ", ts->mux_rate);
    av_log(s, AV_LOG_VERBOSE,
           "pcr every %d pkts, sdt every %d, pat/pmt every %d pkts\n, nit every %d pkts\n, tot every %d pkts\n",
           ts_st->service->pcr_packet_period,
           ts->sdt_packet_period, 
           ts->pat_packet_period,
           ts->nit_packet_period,
           ts->tot_packet_period,
           ts->eit_packet_period);

    if (ts->m2ts_mode == -1) {
        if (av_match_ext(s->filename, "m2ts")) {
            ts->m2ts_mode = 1;
        } else {
            ts->m2ts_mode = 0;
        }
    }

    return 0;

fail:
    av_freep(&pids);
    return ret;
}

/* send SDT, PAT and PMT tables regulary */
static void retransmit_si_info(AVFormatContext *s, int force_pat, int64_t dts)
{
    MpegTSWrite *ts = s->priv_data;
    int i;

    if (++ts->sdt_packet_count == ts->sdt_packet_period ||
        (dts != AV_NOPTS_VALUE && ts->last_sdt_ts == AV_NOPTS_VALUE) ||
        (dts != AV_NOPTS_VALUE && dts - ts->last_sdt_ts >= ts->sdt_period*90000.0)
    ) {
        ts->sdt_packet_count = 0;
        if (dts != AV_NOPTS_VALUE)
            ts->last_sdt_ts = FFMAX(dts, ts->last_sdt_ts);
        mpegts_write_sdt(s);
    }

    if (++ts->nit_packet_count == ts->nit_packet_period ||
        (dts != AV_NOPTS_VALUE && ts->last_nit_ts == AV_NOPTS_VALUE) ||
        (dts != AV_NOPTS_VALUE && dts - ts->last_nit_ts >= ts->nit_period*90000.0)
    ) {
        ts->nit_packet_count = 0;
        if (dts != AV_NOPTS_VALUE)
            ts->last_nit_ts = FFMAX(dts, ts->last_nit_ts);
        mpegts_write_nit(s);
    }


    if (++ts->tot_packet_count == ts->tot_packet_period ||
        (dts != AV_NOPTS_VALUE && ts->last_tot_ts == AV_NOPTS_VALUE) ||
        (dts != AV_NOPTS_VALUE && dts - ts->last_tot_ts >= ts->tot_period*90000.0)
    ) {
        ts->tot_packet_count = 0;
        if (dts != AV_NOPTS_VALUE)
            ts->last_tot_ts = FFMAX(dts, ts->last_tot_ts);
        mpegts_write_tot(s);
    }


    if (++ts->eit_packet_count == ts->eit_packet_period ||
        (dts != AV_NOPTS_VALUE && ts->last_eit_ts == AV_NOPTS_VALUE) ||
        (dts != AV_NOPTS_VALUE && dts - ts->last_eit_ts >= ts->eit_period*90000.0)
    ) {
        ts->eit_packet_count = 0;
        if (dts != AV_NOPTS_VALUE)
            ts->last_eit_ts = FFMAX(dts, ts->last_eit_ts);
        mpegts_write_eit(s);
    }

    if (++ts->pat_packet_count == ts->pat_packet_period ||
        (dts != AV_NOPTS_VALUE && ts->last_pat_ts == AV_NOPTS_VALUE) ||
        (dts != AV_NOPTS_VALUE && dts - ts->last_pat_ts >= ts->pat_period*90000.0) ||
        force_pat) {
        ts->pat_packet_count = 0;
        if (dts != AV_NOPTS_VALUE)
            ts->last_pat_ts = FFMAX(dts, ts->last_pat_ts);
        mpegts_write_pat(s);
        for (i = 0; i < ts->nb_services; i++)
            mpegts_write_pmt(s, ts->services[i]);
    }
}

static int write_pcr_bits(uint8_t *buf, int64_t pcr)
{
    int64_t pcr_low = pcr % 300, pcr_high = pcr / 300;

    *buf++ = pcr_high >> 25;
    *buf++ = pcr_high >> 17;
    *buf++ = pcr_high >>  9;
    *buf++ = pcr_high >>  1;
    *buf++ = pcr_high <<  7 | pcr_low >> 8 | 0x7e;
    *buf++ = pcr_low;

    return 6;
}

/* Write a single null transport stream packet */
static void mpegts_insert_null_packet(AVFormatContext *s)
{
    uint8_t *q;
    uint8_t buf[TS_PACKET_SIZE];

    q    = buf;
    *q++ = 0x47;
    *q++ = 0x00 | 0x1f;
    *q++ = 0xff;
    *q++ = 0x10;
    memset(q, 0x0FF, TS_PACKET_SIZE - (q - buf));
    mpegts_prefix_m2ts_header(s);
    avio_write(s->pb, buf, TS_PACKET_SIZE);
}

/* Write a single transport stream packet with a PCR and no payload */
static void mpegts_insert_pcr_only(AVFormatContext *s, AVStream *st)
{
    MpegTSWrite *ts = s->priv_data;
    MpegTSWriteStream *ts_st = st->priv_data;
    uint8_t *q;
    uint8_t buf[TS_PACKET_SIZE];

    q    = buf;
    *q++ = 0x47;
    *q++ = ts_st->pid >> 8;
    *q++ = ts_st->pid;
    *q++ = 0x20 | ts_st->cc;   /* Adaptation only */
    /* Continuity Count field does not increment (see 13818-1 section 2.4.3.3) */
    *q++ = TS_PACKET_SIZE - 5; /* Adaptation Field Length */
    *q++ = 0x10;               /* Adaptation flags: PCR present */

    /* PCR coded into 6 bytes */
    q += write_pcr_bits(q, get_pcr(ts, s->pb));

    /* stuffing bytes */
    memset(q, 0xFF, TS_PACKET_SIZE - (q - buf));
    mpegts_prefix_m2ts_header(s);
    avio_write(s->pb, buf, TS_PACKET_SIZE);
}

static void write_pts(uint8_t *q, int fourbits, int64_t pts)
{
    int val;

    val  = fourbits << 4 | (((pts >> 30) & 0x07) << 1) | 1;
    *q++ = val;
    val  = (((pts >> 15) & 0x7fff) << 1) | 1;
    *q++ = val >> 8;
    *q++ = val;
    val  = (((pts) & 0x7fff) << 1) | 1;
    *q++ = val >> 8;
    *q++ = val;
}

/* Set an adaptation field flag in an MPEG-TS packet*/
static void set_af_flag(uint8_t *pkt, int flag)
{
    // expect at least one flag to set
    av_assert0(flag);

    if ((pkt[3] & 0x20) == 0) {
        // no AF yet, set adaptation field flag
        pkt[3] |= 0x20;
        // 1 byte length, no flags
        pkt[4] = 1;
        pkt[5] = 0;
    }
    pkt[5] |= flag;
}

/* Extend the adaptation field by size bytes */
static void extend_af(uint8_t *pkt, int size)
{
    // expect already existing adaptation field
    av_assert0(pkt[3] & 0x20);
    pkt[4] += size;
}

/* Get a pointer to MPEG-TS payload (right after TS packet header) */
static uint8_t *get_ts_payload_start(uint8_t *pkt)
{
    if (pkt[3] & 0x20)
        return pkt + 5 + pkt[4];
    else
        return pkt + 4;
}

/* Add a PES header to the front of the payload, and segment into an integer
 * number of TS packets. The final TS packet is padded using an oversized
 * adaptation header to exactly fill the last TS packet.
 * NOTE: 'payload' contains a complete PES payload. */
static void mpegts_write_pes(AVFormatContext *s, AVStream *st,
                             const uint8_t *payload, int payload_size,
                             int64_t pts, int64_t dts, int key, int stream_id)
{
    MpegTSWriteStream *ts_st = st->priv_data;
    MpegTSWrite *ts = s->priv_data;
    uint8_t buf[TS_PACKET_SIZE];
    uint8_t *q;
    int val, is_start, len, header_len, write_pcr, is_dvb_subtitle, is_dvb_teletext, flags;
    int afc_len, stuffing_len;
    int64_t pcr = -1; /* avoid warning */
    int64_t delay = av_rescale(s->max_delay, 90000, AV_TIME_BASE);
    int force_pat = st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && key && !ts_st->prev_payload_key;

    av_assert0(ts_st->payload != buf || st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO);
    if (ts->flags & MPEGTS_FLAG_PAT_PMT_AT_FRAMES && st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
        force_pat = 1;
    }

    is_start = 1;
    while (payload_size > 0) {
        retransmit_si_info(s, force_pat, dts);
        force_pat = 0;

        write_pcr = 0;
        if (ts_st->pid == ts_st->service->pcr_pid) {
            if (ts->mux_rate > 1 || is_start) // VBR pcr period is based on frames
                ts_st->service->pcr_packet_count++;
            if (ts_st->service->pcr_packet_count >=
                ts_st->service->pcr_packet_period) {
                ts_st->service->pcr_packet_count = 0;
                write_pcr = 1;
            }
        }

        if (ts->mux_rate > 1 && dts != AV_NOPTS_VALUE &&
            (dts - get_pcr(ts, s->pb) / 300) > delay) {
            /* pcr insert gets priority over null packet insert */
            if (write_pcr)
                mpegts_insert_pcr_only(s, st);
            else
                mpegts_insert_null_packet(s);
            /* recalculate write_pcr and possibly retransmit si_info */
            continue;
        }

        /* prepare packet header */
        q    = buf;
        *q++ = 0x47;
        val  = ts_st->pid >> 8;
        if (is_start)
            val |= 0x40;
        *q++      = val;
        *q++      = ts_st->pid;
        ts_st->cc = ts_st->cc + 1 & 0xf;
        *q++      = 0x10 | ts_st->cc; // payload indicator + CC
        if (key && is_start && pts != AV_NOPTS_VALUE) {
            // set Random Access for key frames
            if (ts_st->pid == ts_st->service->pcr_pid)
                write_pcr = 1;
            set_af_flag(buf, 0x40);
            q = get_ts_payload_start(buf);
        }
        if (write_pcr) {
            set_af_flag(buf, 0x10);
            q = get_ts_payload_start(buf);
            // add 11, pcr references the last byte of program clock reference base
            if (ts->mux_rate > 1)
                pcr = get_pcr(ts, s->pb);
            else
                pcr = (dts - delay) * 300;
            if (dts != AV_NOPTS_VALUE && dts < pcr / 300)
                av_log(s, AV_LOG_WARNING, "dts < pcr, TS is invalid\n");
            extend_af(buf, write_pcr_bits(q, pcr));
            q = get_ts_payload_start(buf);
        }
        if (is_start) {
            int pes_extension = 0;
            int pes_header_stuffing_bytes = 0;
            /* write PES header */
            *q++ = 0x00;
            *q++ = 0x00;
            *q++ = 0x01;
            is_dvb_subtitle = 0;
            is_dvb_teletext = 0;
            if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                if (st->codecpar->codec_id == AV_CODEC_ID_DIRAC)
                    *q++ = 0xfd;
                else
                    *q++ = 0xe0;
            } else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
                       (st->codecpar->codec_id == AV_CODEC_ID_MP2 ||
                        st->codecpar->codec_id == AV_CODEC_ID_MP3 ||
                        st->codecpar->codec_id == AV_CODEC_ID_AAC)) {
                *q++ = 0xc0;
            } else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
                        st->codecpar->codec_id == AV_CODEC_ID_AC3 &&
                        ts->m2ts_mode) {
                *q++ = 0xfd;
            } else if (st->codecpar->codec_type == AVMEDIA_TYPE_DATA) {
                *q++ = stream_id != -1 ? stream_id : 0xfc;

                if (stream_id == 0xbd) /* asynchronous KLV */
                    pts = dts = AV_NOPTS_VALUE;
            } else {
                *q++ = 0xbd;
                if (st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
                    if (st->codecpar->codec_id == AV_CODEC_ID_DVB_SUBTITLE) {
                        is_dvb_subtitle = 1;
                    } else if (st->codecpar->codec_id == AV_CODEC_ID_DVB_TELETEXT) {
                        is_dvb_teletext = 1;
                    }
                }
            }
            header_len = 0;
            flags      = 0;
            if (pts != AV_NOPTS_VALUE) {
                header_len += 5;
                flags      |= 0x80;
            }
            if (dts != AV_NOPTS_VALUE && pts != AV_NOPTS_VALUE && dts != pts) {
                header_len += 5;
                flags      |= 0x40;
            }
            if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO &&
                st->codecpar->codec_id == AV_CODEC_ID_DIRAC) {
                /* set PES_extension_flag */
                pes_extension = 1;
                flags        |= 0x01;

                /* One byte for PES2 extension flag +
                 * one byte for extension length +
                 * one byte for extension id */
                header_len += 3;
            }
            /* for Blu-ray AC3 Audio the PES Extension flag should be as follow
             * otherwise it will not play sound on blu-ray
             */
            if (ts->m2ts_mode &&
                st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
                st->codecpar->codec_id == AV_CODEC_ID_AC3) {
                        /* set PES_extension_flag */
                        pes_extension = 1;
                        flags |= 0x01;
                        header_len += 3;
            }
            if (is_dvb_teletext) {
                pes_header_stuffing_bytes = 0x24 - header_len;
                header_len = 0x24;
            }
            len = payload_size + header_len + 3;
            /* 3 extra bytes should be added to DVB subtitle payload: 0x20 0x00 at the beginning and trailing 0xff */
            if (is_dvb_subtitle) {
                len += 3;
                payload_size++;
            }
            if (len > 0xffff)
                len = 0;
            if (ts->omit_video_pes_length && st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                len = 0;
            }
            *q++ = len >> 8;
            *q++ = len;
            val  = 0x80;
            /* data alignment indicator is required for subtitle and data streams */
            if (st->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE || st->codecpar->codec_type == AVMEDIA_TYPE_DATA)
                val |= 0x04;
            *q++ = val;
            *q++ = flags;
            *q++ = header_len;
            if (pts != AV_NOPTS_VALUE) {
                write_pts(q, flags >> 6, pts);
                q += 5;
            }
            if (dts != AV_NOPTS_VALUE && pts != AV_NOPTS_VALUE && dts != pts) {
                write_pts(q, 1, dts);
                q += 5;
            }
            if (pes_extension && st->codecpar->codec_id == AV_CODEC_ID_DIRAC) {
                flags = 0x01;  /* set PES_extension_flag_2 */
                *q++  = flags;
                *q++  = 0x80 | 0x01; /* marker bit + extension length */
                /* Set the stream ID extension flag bit to 0 and
                 * write the extended stream ID. */
                *q++ = 0x00 | 0x60;
            }
            /* For Blu-ray AC3 Audio Setting extended flags */
          if (ts->m2ts_mode &&
              pes_extension &&
              st->codecpar->codec_id == AV_CODEC_ID_AC3) {
                      flags = 0x01; /* set PES_extension_flag_2 */
                      *q++ = flags;
                      *q++ = 0x80 | 0x01; /* marker bit + extension length */
                      *q++ = 0x00 | 0x71; /* for AC3 Audio (specifically on blue-rays) */
              }


            if (is_dvb_subtitle) {
                /* First two fields of DVB subtitles PES data:
                 * data_identifier: for DVB subtitle streams shall be coded with the value 0x20
                 * subtitle_stream_id: for DVB subtitle stream shall be identified by the value 0x00 */
                *q++ = 0x20;
                *q++ = 0x00;
            }
            if (is_dvb_teletext) {
                memset(q, 0xff, pes_header_stuffing_bytes);
                q += pes_header_stuffing_bytes;
            }
            is_start = 0;
        }
        /* header size */
        header_len = q - buf;
        /* data len */
        len = TS_PACKET_SIZE - header_len;
        if (len > payload_size)
            len = payload_size;
        stuffing_len = TS_PACKET_SIZE - header_len - len;
        if (stuffing_len > 0) {
            /* add stuffing with AFC */
            if (buf[3] & 0x20) {
                /* stuffing already present: increase its size */
                afc_len = buf[4] + 1;
                memmove(buf + 4 + afc_len + stuffing_len,
                        buf + 4 + afc_len,
                        header_len - (4 + afc_len));
                buf[4] += stuffing_len;
                memset(buf + 4 + afc_len, 0xff, stuffing_len);
            } else {
                /* add stuffing */
                memmove(buf + 4 + stuffing_len, buf + 4, header_len - 4);
                buf[3] |= 0x20;
                buf[4]  = stuffing_len - 1;
                if (stuffing_len >= 2) {
                    buf[5] = 0x00;
                    memset(buf + 6, 0xff, stuffing_len - 2);
                }
            }
        }

        if (is_dvb_subtitle && payload_size == len) {
            memcpy(buf + TS_PACKET_SIZE - len, payload, len - 1);
            buf[TS_PACKET_SIZE - 1] = 0xff; /* end_of_PES_data_field_marker: an 8-bit field with fixed contents 0xff for DVB subtitle */
        } else {
            memcpy(buf + TS_PACKET_SIZE - len, payload, len);
        }

        payload      += len;
        payload_size -= len;
        mpegts_prefix_m2ts_header(s);
        avio_write(s->pb, buf, TS_PACKET_SIZE);
    }
    ts_st->prev_payload_key = key;
}

int ff_check_h264_startcode(AVFormatContext *s, const AVStream *st, const AVPacket *pkt)
{
    if (pkt->size < 5 || AV_RB32(pkt->data) != 0x0000001 && AV_RB24(pkt->data) != 0x000001) {
        if (!st->nb_frames) {
            av_log(s, AV_LOG_ERROR, "H.264 bitstream malformed, "
                   "no startcode found, use the video bitstream filter 'h264_mp4toannexb' to fix it "
                   "('-bsf:v h264_mp4toannexb' option with ffmpeg)\n");
            return AVERROR_INVALIDDATA;
        }
        av_log(s, AV_LOG_WARNING, "H.264 bitstream error, startcode missing, size %d", pkt->size);
        if (pkt->size) av_log(s, AV_LOG_WARNING, " data %08X", AV_RB32(pkt->data));
        av_log(s, AV_LOG_WARNING, "\n");
    }
    return 0;
}

static int check_hevc_startcode(AVFormatContext *s, const AVStream *st, const AVPacket *pkt)
{
    if (pkt->size < 5 || AV_RB32(pkt->data) != 0x0000001 && AV_RB24(pkt->data) != 0x000001) {
        if (!st->nb_frames) {
            av_log(s, AV_LOG_ERROR, "HEVC bitstream malformed, no startcode found\n");
            return AVERROR_PATCHWELCOME;
        }
        av_log(s, AV_LOG_WARNING, "HEVC bitstream error, startcode missing, size %d", pkt->size);
        if (pkt->size) av_log(s, AV_LOG_WARNING, " data %08X", AV_RB32(pkt->data));
        av_log(s, AV_LOG_WARNING, "\n");
    }
    return 0;
}

/* Based on GStreamer's gst-plugins-base/ext/ogg/gstoggstream.c
 * Released under the LGPL v2.1+, written by
 * Vincent Penquerc'h <vincent.penquerch@collabora.co.uk>
 */
static int opus_get_packet_samples(AVFormatContext *s, AVPacket *pkt)
{
    static const int durations[32] = {
      480, 960, 1920, 2880,       /* Silk NB */
      480, 960, 1920, 2880,       /* Silk MB */
      480, 960, 1920, 2880,       /* Silk WB */
      480, 960,                   /* Hybrid SWB */
      480, 960,                   /* Hybrid FB */
      120, 240, 480, 960,         /* CELT NB */
      120, 240, 480, 960,         /* CELT NB */
      120, 240, 480, 960,         /* CELT NB */
      120, 240, 480, 960,         /* CELT NB */
    };
    int toc, frame_duration, nframes, duration;

    if (pkt->size < 1)
        return 0;

    toc = pkt->data[0];

    frame_duration = durations[toc >> 3];
    switch (toc & 3) {
    case 0:
        nframes = 1;
        break;
    case 1:
        nframes = 2;
        break;
    case 2:
        nframes = 2;
        break;
    case 3:
        if (pkt->size < 2)
            return 0;
        nframes = pkt->data[1] & 63;
        break;
    }

    duration = nframes * frame_duration;
    if (duration > 5760) {
        av_log(s, AV_LOG_WARNING,
               "Opus packet duration > 120 ms, invalid");
        return 0;
    }

    return duration;
}

static int mpegts_write_packet_internal(AVFormatContext *s, AVPacket *pkt)
{
    AVStream *st = s->streams[pkt->stream_index];
    int size = pkt->size;
    uint8_t *buf = pkt->data;
    uint8_t *data = NULL;
    MpegTSWrite *ts = s->priv_data;
    MpegTSWriteStream *ts_st = st->priv_data;
    const int64_t delay = av_rescale(s->max_delay, 90000, AV_TIME_BASE) * 2;
    int64_t dts = pkt->dts, pts = pkt->pts;
    int opus_samples = 0;
    int side_data_size;
    char *side_data = NULL;
    int stream_id = -1;

    side_data = av_packet_get_side_data(pkt,
                                        AV_PKT_DATA_MPEGTS_STREAM_ID,
                                        &side_data_size);
    if (side_data)
        stream_id = side_data[0];

    if (ts->reemit_pat_pmt) {
        av_log(s, AV_LOG_WARNING,
               "resend_headers option is deprecated, use -mpegts_flags resend_headers\n");
        ts->reemit_pat_pmt = 0;
        ts->flags         |= MPEGTS_FLAG_REEMIT_PAT_PMT;
    }

    if (ts->flags & MPEGTS_FLAG_REEMIT_PAT_PMT) {
        ts->pat_packet_count = ts->pat_packet_period - 1;
        ts->sdt_packet_count = ts->sdt_packet_period - 1;
        ts->nit_packet_count = ts->nit_packet_period - 1;
        ts->tot_packet_count = ts->tot_packet_period - 1;
        ts->eit_packet_count = ts->eit_packet_period - 1;
        ts->flags           &= ~MPEGTS_FLAG_REEMIT_PAT_PMT;
    }

    if (ts->copyts < 1) {
        if (pts != AV_NOPTS_VALUE)
            pts += delay;
        if (dts != AV_NOPTS_VALUE)
            dts += delay;
    }

    if (ts_st->first_pts_check && pts == AV_NOPTS_VALUE) {
        av_log(s, AV_LOG_ERROR, "first pts value must be set\n");
        return AVERROR_INVALIDDATA;
    }
    ts_st->first_pts_check = 0;

    if (st->codecpar->codec_id == AV_CODEC_ID_H264) {
        const uint8_t *p = buf, *buf_end = p + size;
        uint32_t state = -1;
        int extradd = (pkt->flags & AV_PKT_FLAG_KEY) ? st->codecpar->extradata_size : 0;
        int ret = ff_check_h264_startcode(s, st, pkt);
        if (ret < 0)
            return ret;

        if (extradd && AV_RB24(st->codecpar->extradata) > 1)
            extradd = 0;

        do {
            p = avpriv_find_start_code(p, buf_end, &state);
            av_log(s, AV_LOG_TRACE, "nal %d\n", state & 0x1f);
            if ((state & 0x1f) == 7)
                extradd = 0;
        } while (p < buf_end && (state & 0x1f) != 9 &&
                 (state & 0x1f) != 5 && (state & 0x1f) != 1);

        if ((state & 0x1f) != 5)
            extradd = 0;
        if ((state & 0x1f) != 9) { // AUD NAL
            data = av_malloc(pkt->size + 6 + extradd);
            if (!data)
                return AVERROR(ENOMEM);
            memcpy(data + 6, st->codecpar->extradata, extradd);
            memcpy(data + 6 + extradd, pkt->data, pkt->size);
            AV_WB32(data, 0x00000001);
            data[4] = 0x09;
            data[5] = 0xf0; // any slice type (0xe) + rbsp stop one bit
            buf     = data;
            size    = pkt->size + 6 + extradd;
        }
    } else if (st->codecpar->codec_id == AV_CODEC_ID_AAC) {
        if (pkt->size < 2) {
            av_log(s, AV_LOG_ERROR, "AAC packet too short\n");
            return AVERROR_INVALIDDATA;
        }
        if ((AV_RB16(pkt->data) & 0xfff0) != 0xfff0) {
            int ret;
            AVPacket pkt2;

            if (!ts_st->amux) {
                av_log(s, AV_LOG_ERROR, "AAC bitstream not in ADTS format "
                                        "and extradata missing\n");
            } else {
            av_init_packet(&pkt2);
            pkt2.data = pkt->data;
            pkt2.size = pkt->size;
            av_assert0(pkt->dts != AV_NOPTS_VALUE);
            pkt2.dts = av_rescale_q(pkt->dts, st->time_base, ts_st->amux->streams[0]->time_base);

            ret = avio_open_dyn_buf(&ts_st->amux->pb);
            if (ret < 0)
                return AVERROR(ENOMEM);

            ret = av_write_frame(ts_st->amux, &pkt2);
            if (ret < 0) {
                ffio_free_dyn_buf(&ts_st->amux->pb);
                return ret;
            }
            size            = avio_close_dyn_buf(ts_st->amux->pb, &data);
            ts_st->amux->pb = NULL;
            buf             = data;
            }
        }
    } else if (st->codecpar->codec_id == AV_CODEC_ID_HEVC) {
        int ret = check_hevc_startcode(s, st, pkt);
        if (ret < 0)
            return ret;
    } else if (st->codecpar->codec_id == AV_CODEC_ID_OPUS) {
        if (pkt->size < 2) {
            av_log(s, AV_LOG_ERROR, "Opus packet too short\n");
            return AVERROR_INVALIDDATA;
        }

        /* Add Opus control header */
        if ((AV_RB16(pkt->data) >> 5) != 0x3ff) {
            uint8_t *side_data;
            int side_data_size;
            int i, n;
            int ctrl_header_size;
            int trim_start = 0, trim_end = 0;

            opus_samples = opus_get_packet_samples(s, pkt);

            side_data = av_packet_get_side_data(pkt,
                                                AV_PKT_DATA_SKIP_SAMPLES,
                                                &side_data_size);

            if (side_data && side_data_size >= 10) {
                trim_end = AV_RL32(side_data + 4) * 48000 / st->codecpar->sample_rate;
            }

            ctrl_header_size = pkt->size + 2 + pkt->size / 255 + 1;
            if (ts_st->opus_pending_trim_start)
              ctrl_header_size += 2;
            if (trim_end)
              ctrl_header_size += 2;

            data = av_malloc(ctrl_header_size);
            if (!data)
                return AVERROR(ENOMEM);

            data[0] = 0x7f;
            data[1] = 0xe0;
            if (ts_st->opus_pending_trim_start)
                data[1] |= 0x10;
            if (trim_end)
                data[1] |= 0x08;

            n = pkt->size;
            i = 2;
            do {
                data[i] = FFMIN(n, 255);
                n -= 255;
                i++;
            } while (n >= 0);

            av_assert0(2 + pkt->size / 255 + 1 == i);

            if (ts_st->opus_pending_trim_start) {
                trim_start = FFMIN(ts_st->opus_pending_trim_start, opus_samples);
                AV_WB16(data + i, trim_start);
                i += 2;
                ts_st->opus_pending_trim_start -= trim_start;
            }
            if (trim_end) {
                trim_end = FFMIN(trim_end, opus_samples - trim_start);
                AV_WB16(data + i, trim_end);
                i += 2;
            }

            memcpy(data + i, pkt->data, pkt->size);
            buf     = data;
            size    = ctrl_header_size;
        } else {
            /* TODO: Can we get TS formatted data here? If so we will
             * need to count the samples of that too! */
            av_log(s, AV_LOG_WARNING, "Got MPEG-TS formatted Opus data, unhandled");
        }
    }

    if (pkt->dts != AV_NOPTS_VALUE) {
        int i;
        for(i=0; i<s->nb_streams; i++) {
            AVStream *st2 = s->streams[i];
            MpegTSWriteStream *ts_st2 = st2->priv_data;
            if (   ts_st2->payload_size
               && (ts_st2->payload_dts == AV_NOPTS_VALUE || dts - ts_st2->payload_dts > delay/2)) {
                mpegts_write_pes(s, st2, ts_st2->payload, ts_st2->payload_size,
                                 ts_st2->payload_pts, ts_st2->payload_dts,
                                 ts_st2->payload_flags & AV_PKT_FLAG_KEY, stream_id);
                ts_st2->payload_size = 0;
            }
        }
    }

    if (ts_st->payload_size && (ts_st->payload_size + size > ts->pes_payload_size ||
        (dts != AV_NOPTS_VALUE && ts_st->payload_dts != AV_NOPTS_VALUE &&
         av_compare_ts(dts - ts_st->payload_dts, st->time_base,
                       s->max_delay, AV_TIME_BASE_Q) >= 0) ||
        ts_st->opus_queued_samples + opus_samples >= 5760 /* 120ms */)) {
        mpegts_write_pes(s, st, ts_st->payload, ts_st->payload_size,
                         ts_st->payload_pts, ts_st->payload_dts,
                         ts_st->payload_flags & AV_PKT_FLAG_KEY, stream_id);
        ts_st->payload_size = 0;
        ts_st->opus_queued_samples = 0;
    }

    if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO || size > ts->pes_payload_size) {
        av_assert0(!ts_st->payload_size);
        // for video and subtitle, write a single pes packet
        mpegts_write_pes(s, st, buf, size, pts, dts,
                         pkt->flags & AV_PKT_FLAG_KEY, stream_id);
        ts_st->opus_queued_samples = 0;
        av_free(data);
        return 0;
    }

    if (!ts_st->payload_size) {
        ts_st->payload_pts   = pts;
        ts_st->payload_dts   = dts;
        ts_st->payload_flags = pkt->flags;
    }

    memcpy(ts_st->payload + ts_st->payload_size, buf, size);
    ts_st->payload_size += size;
    ts_st->opus_queued_samples += opus_samples;

    av_free(data);

    return 0;
}

static void mpegts_write_flush(AVFormatContext *s)
{
    int i;

    /* flush current packets */
    for (i = 0; i < s->nb_streams; i++) {
        AVStream *st = s->streams[i];
        MpegTSWriteStream *ts_st = st->priv_data;
        if (ts_st->payload_size > 0) {
            mpegts_write_pes(s, st, ts_st->payload, ts_st->payload_size,
                             ts_st->payload_pts, ts_st->payload_dts,
                             ts_st->payload_flags & AV_PKT_FLAG_KEY, -1);
            ts_st->payload_size = 0;
            ts_st->opus_queued_samples = 0;
        }
    }
}

static int mpegts_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    if (!pkt) {
        mpegts_write_flush(s);
        return 1;
    } else {
        return mpegts_write_packet_internal(s, pkt);
    }
}

static int mpegts_write_end(AVFormatContext *s)
{
    if (s->pb)
        mpegts_write_flush(s);

    return 0;
}

static void mpegts_deinit(AVFormatContext *s)
{
    MpegTSWrite *ts = s->priv_data;
    MpegTSService *service;
    int i;

    for (i = 0; i < s->nb_streams; i++) {
        AVStream *st = s->streams[i];
        MpegTSWriteStream *ts_st = st->priv_data;
        if (ts_st) {
            av_freep(&ts_st->payload);
            if (ts_st->amux) {
                avformat_free_context(ts_st->amux);
                ts_st->amux = NULL;
            }
        }
    }

    for (i = 0; i < ts->nb_services; i++) {
        service = ts->services[i];
        av_freep(&service->provider_name);
        av_freep(&service->name);
        av_freep(&service);
    }
    av_freep(&ts->services);
}

static int mpegts_check_bitstream(struct AVFormatContext *s, const AVPacket *pkt)
{
    int ret = 1;
    AVStream *st = s->streams[pkt->stream_index];

    if (st->codecpar->codec_id == AV_CODEC_ID_H264) {
        if (pkt->size >= 5 && AV_RB32(pkt->data) != 0x0000001 &&
                              AV_RB24(pkt->data) != 0x000001)
            ret = ff_stream_add_bitstream_filter(st, "h264_mp4toannexb", NULL);
    } else if (st->codecpar->codec_id == AV_CODEC_ID_HEVC) {
        if (pkt->size >= 5 && AV_RB32(pkt->data) != 0x0000001 &&
                              AV_RB24(pkt->data) != 0x000001)
            ret = ff_stream_add_bitstream_filter(st, "hevc_mp4toannexb", NULL);
    }

    return ret;
}

static const AVOption options[] = {
    { "mpegts_transport_stream_id", "Set transport_stream_id field.",
      offsetof(MpegTSWrite, transport_stream_id), AV_OPT_TYPE_INT,
      { .i64 = 0x0001 }, 0x0001, 0xffff, AV_OPT_FLAG_ENCODING_PARAM },
    { "mpegts_original_network_id", "Set original_network_id field.",
      offsetof(MpegTSWrite, original_network_id), AV_OPT_TYPE_INT,
      { .i64 = 0x0001 }, 0x0001, 0xffff, AV_OPT_FLAG_ENCODING_PARAM },
    { "mpegts_service_id", "Set service_id field.",
      offsetof(MpegTSWrite, service_id), AV_OPT_TYPE_INT,
      { .i64 = 0x0001 }, 0x0001, 0xffff, AV_OPT_FLAG_ENCODING_PARAM },

    { "mpegts_final_nb_services", "Set desired number of services.",
      offsetof(MpegTSWrite, final_nb_services), AV_OPT_TYPE_INT, {.i64 = 0x0001 }, 0x0001, 0x0005, AV_OPT_FLAG_ENCODING_PARAM},
    { "mpegts_area_code", "Set area_code field.",
      offsetof(MpegTSWrite, area_code), AV_OPT_TYPE_INT, {.i64 = 0x0001 }, 0x0001, 0x0DBF, AV_OPT_FLAG_ENCODING_PARAM},
    { "mpegts_guard_interval", "Set guard_interval  field.",
      offsetof(MpegTSWrite, guard_interval), AV_OPT_TYPE_INT, {.i64 = 0x0001 }, 0x0001, 0x0004, AV_OPT_FLAG_ENCODING_PARAM},
    { "mpegts_transmission_mode", "Set transmission_mode field.",
      offsetof(MpegTSWrite, transmission_mode), AV_OPT_TYPE_INT, {.i64 = 0x0001 }, 0x0001, 0x0004, AV_OPT_FLAG_ENCODING_PARAM},
    { "mpegts_physical_channel", "Set physical_channel field.",
      offsetof(MpegTSWrite, physical_channel), AV_OPT_TYPE_INT, {.i64 = 0x0014 }, 0x000E, 0x0045, AV_OPT_FLAG_ENCODING_PARAM},
    { "mpegts_virtual_channel", "Set virtual_channel field.",
      offsetof(MpegTSWrite, virtual_channel), AV_OPT_TYPE_INT, {.i64 = 0x0014 }, 0x0001, 0x0D45, AV_OPT_FLAG_ENCODING_PARAM},
    { "mpegts_transmission_profile", "Set transmission_profile field.",
      offsetof(MpegTSWrite, transmission_profile), AV_OPT_TYPE_INT, {.i64 = 0x0000 }, 0x0000, 0x0003, AV_OPT_FLAG_ENCODING_PARAM},

    { "mpegts_service_type", "Set service_type field.",
      offsetof(MpegTSWrite, service_type), AV_OPT_TYPE_INT,
      { .i64 = 0x01 }, 0x01, 0xff, AV_OPT_FLAG_ENCODING_PARAM, "mpegts_service_type" },
    { "digital_tv", "Digital Television.",
      0, AV_OPT_TYPE_CONST, { .i64 = MPEGTS_SERVICE_TYPE_DIGITAL_TV }, 0x01, 0xff,
      AV_OPT_FLAG_ENCODING_PARAM, "mpegts_service_type" },
    { "digital_radio", "Digital Radio.",
      0, AV_OPT_TYPE_CONST, { .i64 = MPEGTS_SERVICE_TYPE_DIGITAL_RADIO }, 0x01, 0xff,
      AV_OPT_FLAG_ENCODING_PARAM, "mpegts_service_type" },
    { "teletext", "Teletext.",
      0, AV_OPT_TYPE_CONST, { .i64 = MPEGTS_SERVICE_TYPE_TELETEXT }, 0x01, 0xff,
      AV_OPT_FLAG_ENCODING_PARAM, "mpegts_service_type" },
    { "advanced_codec_digital_radio", "Advanced Codec Digital Radio.",
      0, AV_OPT_TYPE_CONST, { .i64 = MPEGTS_SERVICE_TYPE_ADVANCED_CODEC_DIGITAL_RADIO }, 0x01, 0xff,
      AV_OPT_FLAG_ENCODING_PARAM, "mpegts_service_type" },
    { "mpeg2_digital_hdtv", "MPEG2 Digital HDTV.",
      0, AV_OPT_TYPE_CONST, { .i64 = MPEGTS_SERVICE_TYPE_MPEG2_DIGITAL_HDTV }, 0x01, 0xff,
      AV_OPT_FLAG_ENCODING_PARAM, "mpegts_service_type" },
    { "advanced_codec_digital_sdtv", "Advanced Codec Digital SDTV.",
      0, AV_OPT_TYPE_CONST, { .i64 = MPEGTS_SERVICE_TYPE_ADVANCED_CODEC_DIGITAL_SDTV }, 0x01, 0xff,
      AV_OPT_FLAG_ENCODING_PARAM, "mpegts_service_type" },
    { "advanced_codec_digital_hdtv", "Advanced Codec Digital HDTV.",
      0, AV_OPT_TYPE_CONST, { .i64 = MPEGTS_SERVICE_TYPE_ADVANCED_CODEC_DIGITAL_HDTV }, 0x01, 0xff,
      AV_OPT_FLAG_ENCODING_PARAM, "mpegts_service_type" },
    { "mpegts_pmt_start_pid", "Set the first pid of the PMT.",
      offsetof(MpegTSWrite, pmt_start_pid), AV_OPT_TYPE_INT,
      { .i64 = 0x1000 }, 0x0010, 0x1f00, AV_OPT_FLAG_ENCODING_PARAM },
    { "mpegts_start_pid", "Set the first pid.",
      offsetof(MpegTSWrite, start_pid), AV_OPT_TYPE_INT,
      { .i64 = 0x0100 }, 0x0020, 0x0f00, AV_OPT_FLAG_ENCODING_PARAM },
    { "mpegts_m2ts_mode", "Enable m2ts mode.",
      offsetof(MpegTSWrite, m2ts_mode), AV_OPT_TYPE_BOOL,
      { .i64 = -1 }, -1, 1, AV_OPT_FLAG_ENCODING_PARAM },
    { "muxrate", NULL,
      offsetof(MpegTSWrite, mux_rate), AV_OPT_TYPE_INT,
      { .i64 = 1 }, 0, INT_MAX, AV_OPT_FLAG_ENCODING_PARAM },
    { "pes_payload_size", "Minimum PES packet payload in bytes",
      offsetof(MpegTSWrite, pes_payload_size), AV_OPT_TYPE_INT,
      { .i64 = DEFAULT_PES_PAYLOAD_SIZE }, 0, INT_MAX, AV_OPT_FLAG_ENCODING_PARAM },
    { "mpegts_flags", "MPEG-TS muxing flags",
      offsetof(MpegTSWrite, flags), AV_OPT_TYPE_FLAGS, { .i64 = 0 }, 0, INT_MAX,
      AV_OPT_FLAG_ENCODING_PARAM, "mpegts_flags" },
    { "resend_headers", "Reemit PAT/PMT before writing the next packet",
      0, AV_OPT_TYPE_CONST, { .i64 = MPEGTS_FLAG_REEMIT_PAT_PMT }, 0, INT_MAX,
      AV_OPT_FLAG_ENCODING_PARAM, "mpegts_flags" },
    { "latm", "Use LATM packetization for AAC",
      0, AV_OPT_TYPE_CONST, { .i64 = MPEGTS_FLAG_AAC_LATM }, 0, INT_MAX,
      AV_OPT_FLAG_ENCODING_PARAM, "mpegts_flags" },
    { "pat_pmt_at_frames", "Reemit PAT and PMT at each video frame",
      0, AV_OPT_TYPE_CONST, { .i64 = MPEGTS_FLAG_PAT_PMT_AT_FRAMES}, 0, INT_MAX,
      AV_OPT_FLAG_ENCODING_PARAM, "mpegts_flags" },
    { "system_b", "Conform to System B (DVB) instead of System A (ATSC)",
      0, AV_OPT_TYPE_CONST, { .i64 = MPEGTS_FLAG_SYSTEM_B }, 0, INT_MAX,
      AV_OPT_FLAG_ENCODING_PARAM, "mpegts_flags" },
    // backward compatibility
    { "resend_headers", "Reemit PAT/PMT before writing the next packet",
      offsetof(MpegTSWrite, reemit_pat_pmt), AV_OPT_TYPE_INT,
      { .i64 = 0 }, 0, INT_MAX, AV_OPT_FLAG_ENCODING_PARAM },
    { "mpegts_copyts", "don't offset dts/pts",
      offsetof(MpegTSWrite, copyts), AV_OPT_TYPE_BOOL,
      { .i64 = -1 }, -1, 1, AV_OPT_FLAG_ENCODING_PARAM },
    { "tables_version", "set PAT, PMT and SDT version",
      offsetof(MpegTSWrite, tables_version), AV_OPT_TYPE_INT,
      { .i64 = 0 }, 0, 31, AV_OPT_FLAG_ENCODING_PARAM },
    { "omit_video_pes_length", "Omit the PES packet length for video packets",
      offsetof(MpegTSWrite, omit_video_pes_length), AV_OPT_TYPE_BOOL,
      { .i64 = 1 }, 0, 1, AV_OPT_FLAG_ENCODING_PARAM },
    { "pcr_period", "PCR retransmission time",
      offsetof(MpegTSWrite, pcr_period), AV_OPT_TYPE_INT,
      { .i64 = PCR_RETRANS_TIME }, 0, INT_MAX, AV_OPT_FLAG_ENCODING_PARAM },
    { "pat_period", "PAT/PMT retransmission time limit in seconds",
      offsetof(MpegTSWrite, pat_period), AV_OPT_TYPE_DOUBLE,
      { .dbl = INT_MAX }, 0, INT_MAX, AV_OPT_FLAG_ENCODING_PARAM },
    { "sdt_period", "SDT retransmission time limit in seconds",
      offsetof(MpegTSWrite, sdt_period), AV_OPT_TYPE_DOUBLE,
      { .dbl = INT_MAX }, 0, INT_MAX, AV_OPT_FLAG_ENCODING_PARAM },
    { NULL },
};

static const AVClass mpegts_muxer_class = {
    .class_name = "MPEGTS muxer",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

AVOutputFormat ff_mpegts_muxer = {
    .name           = "mpegts",
    .long_name      = NULL_IF_CONFIG_SMALL("MPEG-TS (MPEG-2 Transport Stream)"),
    .mime_type      = "video/MP2T",
    .extensions     = "ts,m2t,m2ts,mts",
    .priv_data_size = sizeof(MpegTSWrite),
    .audio_codec    = AV_CODEC_ID_MP2,
    .video_codec    = AV_CODEC_ID_MPEG2VIDEO,
    .init           = mpegts_init,
    .write_packet   = mpegts_write_packet,
    .write_trailer  = mpegts_write_end,
    .deinit         = mpegts_deinit,
    .check_bitstream = mpegts_check_bitstream,
    .flags          = AVFMT_ALLOW_FLUSH | AVFMT_VARIABLE_FPS,
    .priv_class     = &mpegts_muxer_class,
};
