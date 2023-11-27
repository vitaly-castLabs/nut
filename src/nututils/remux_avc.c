#include <stdio.h>
#include <stdlib.h>

#include <libnut.h>

static size_t read(void * priv, size_t len, uint8_t * buf) {
    size_t ret = fread(buf, 1, len, (FILE *)priv);
    printf("read %zu bytes (%zu requested)\n", ret, len);
    return ret;
}

void newInfo(void * priv, nut_info_packet_tt * info) {
    printf("New info, count: %d\n", info->count);
}

int main(int argc, char * argv []) {
    FILE * in = fopen(argv[1], "rb");
    if (!in) {
        printf("open file failed\n");
        return -1;
    }

    nut_input_stream_tt input;
    input.priv = in;
    input.read = read;
    input.seek = NULL;
    input.eof = NULL;

	nut_alloc_tt alloc;
    alloc.malloc = malloc;
    alloc.realloc = realloc;
    alloc.free = free;

    nut_demuxer_opts_tt opts;
    opts.input = input;
    opts.alloc = alloc;
    opts.read_index = 0;
    opts.cache_syncpoints = 0;
    opts.info_priv = NULL;
    opts.new_info = newInfo;

    nut_context_tt* demuxer = nut_demuxer_init(&opts);

    if (demuxer == NULL) {
        printf("nut_demuxer_init failed\n");
        return -1;
    }

    unsigned char frameBuf[1024 * 1024];

    nut_stream_header_tt* s;
    int ret;
    if ((ret = nut_read_headers(demuxer, &s, NULL))) {
        printf("nut_read_headers error: %s\n", nut_error(ret));
        nut_demuxer_uninit(demuxer);
        return -1;
    }

    printf("codec-specific %d bytes\n", s->codec_specific_len);
    for (int i = 0; i < s->codec_specific_len; ++i) {
        printf("%02x ", s->codec_specific[i]);
        if (i % 16 == 15) {
            printf("\n");
        }
    }
    printf("\n");

    while (1) {
        nut_packet_tt pd;
        if ((ret = nut_read_next_packet(demuxer, &pd))) {
            printf("nut_read_next_packet error: %s\n", nut_error(ret));
            nut_demuxer_uninit(demuxer);
            return -1;
        }

        int len = pd.len;
        printf("stream: %d, pts: %lld, len: %d, flags: %d\n", pd.stream, pd.pts, pd.len, pd.flags);

        if ((ret = nut_read_frame(demuxer, &pd.len, frameBuf))) {
            printf("nut_read_frame error: %s\n", nut_error(ret));
            nut_demuxer_uninit(demuxer);
            return -1;
        }

        printf("frame:\n");
        for (int i = 0; i < len; ++i) {
            int col = i & 15;
            if (col == 0) {
                int row = i / 16;
                if (row > 3 && (i + 16) < len) {
                    printf("...\n");
                    break;
                }
            }
            printf("%02x ", frameBuf[i]);
            if (col == 15) {
                printf("\n");
            }
        }
        printf("\n");
    }

    fclose(in);
    //fclose(out);

    return 0;
}
