#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include "nut.h"
#include "priv.h"

static int stream_write(void * priv, size_t len, const uint8_t * buf) {
	return fwrite(buf, 1, len, priv);
}

static void flush_buf(output_buffer_t * bc) {
	assert(!bc->is_mem);
	bc->file_pos += bc->osc.write(bc->osc.priv, bc->buf_ptr - bc->buf, bc->buf);
	bc->buf_ptr = bc->buf;
}

static void ready_write_buf(output_buffer_t * bc, int amount) {
	if (bc->write_len - (bc->buf_ptr - bc->buf) > amount) return;

        if (bc->is_mem) {
		int tmp = bc->buf_ptr - bc->buf;
		bc->write_len = tmp + amount + PREALLOC_SIZE;
		bc->buf = realloc(bc->buf, bc->write_len);
		bc->buf_ptr = bc->buf + tmp;
	} else {
		flush_buf(bc);
		if (bc->write_len < amount) {
			free(bc->buf);
			bc->write_len = amount + PREALLOC_SIZE;
			bc->buf_ptr = bc->buf = malloc(bc->write_len);
		}
	}
}

static output_buffer_t * new_mem_buffer() {
	output_buffer_t * bc = malloc(sizeof(output_buffer_t));
	bc->write_len = PREALLOC_SIZE;
	bc->is_mem = 1;
	bc->file_pos = 0;
	bc->buf_ptr = bc->buf = malloc(bc->write_len);
	return bc;
}

static output_buffer_t * new_output_buffer(nut_output_stream_t osc) {
	output_buffer_t * bc = new_mem_buffer();
	bc->is_mem = 0;
	bc->osc = osc;
	if (!bc->osc.write) bc->osc.write = stream_write;
	return bc;
}

static void free_buffer(output_buffer_t * bc) {
	if (!bc) return;
	if (!bc->is_mem) flush_buf(bc);
	free(bc->buf);
	free(bc);
}

static output_buffer_t * clear_buffer(output_buffer_t * bc) {
	assert(bc->is_mem);
	bc->buf_ptr = bc->buf;
	return bc;
}

static void put_bytes(output_buffer_t * bc, int count, uint64_t val) {
	ready_write_buf(bc, count);
	for(count--; count >= 0; count--){
		*(bc->buf_ptr++) = val >> (8 * count);
	}
}

static int v_len(uint64_t val) {
	int i;
	val &= 0x7FFFFFFFFFFFFFFFULL;
	for(i=1; val>>(i*7); i++);
	return i;
}

static void put_v(output_buffer_t * bc, uint64_t val) {
	int i = v_len(val);
	ready_write_buf(bc, i);
	for(i = (i-1)*7; i; i-=7) {
		*(bc->buf_ptr++) = 0x80 | (val>>i);
	}
	*(bc->buf_ptr++)= val & 0x7F;
}

static void put_s(output_buffer_t * bc, int64_t val) {
	if (val<=0) put_v(bc, -2*val  );
	else        put_v(bc,  2*val-1);
}

static void put_data(output_buffer_t * bc, int len, const uint8_t * data) {
	if (!len) return;
	assert(data);
	if (bc->write_len - (bc->buf_ptr - bc->buf) > len || bc->is_mem) {
		ready_write_buf(bc, len);
		memcpy(bc->buf_ptr, data, len);
		bc->buf_ptr += len;
	} else {
		flush_buf(bc);
		bc->file_pos += bc->osc.write(bc->osc.priv, len, data);
	}
}

static void put_vb(output_buffer_t * bc, int len, uint8_t * data) {
	put_v(bc, len);
	put_data(bc, len, data);
}

static void put_header(output_buffer_t * bc, output_buffer_t * in, output_buffer_t * tmp, uint64_t startcode, int index_ptr) {
	int forward_ptr;
	assert(in->is_mem);
	assert(tmp->is_mem);
	clear_buffer(tmp);

	forward_ptr = bctello(in);
	forward_ptr += 4; // checksum
	if (index_ptr) forward_ptr += 8;

	// packet_header
	put_bytes(tmp, 8, startcode);
	put_v(tmp, forward_ptr);
	if (forward_ptr > 4096) put_bytes(tmp, 4, crc32(tmp->buf, bctello(tmp)));
	put_data(bc, bctello(tmp), tmp->buf);

	// packet_footer
	if (index_ptr) put_bytes(in, 8, bctello(tmp) + bctello(in) + 8 + 4);
	put_bytes(in, 4, crc32(in->buf, bctello(in)));

	put_data(bc, bctello(in), in->buf);
	if (startcode != SYNCPOINT_STARTCODE) fprintf(stderr, "header/index size: %d\n", (int)(bctello(tmp) + bctello(in)));
}

static void put_main_header(nut_context_t * nut) {
	output_buffer_t * tmp = clear_buffer(nut->tmp_buffer);
	int i;
	int flag, fields, timestamp = 0, mul = 1, stream = 0, size, count;

	put_v(tmp, NUT_VERSION);
	put_v(tmp, nut->stream_count);
	put_v(tmp, nut->max_distance);
	for(i = 0; i < 256; ) {
		fields = 0;
		flag = nut->ft[i].flags;
		if (nut->ft[i].pts_delta != timestamp) fields = 1;
		timestamp = nut->ft[i].pts_delta;
		if (nut->ft[i].mul != mul) fields = 2;
		mul = nut->ft[i].mul;
		if (nut->ft[i].stream_plus1 != stream) fields = 3;
		stream = nut->ft[i].stream_plus1;
		if (nut->ft[i].lsb != 0) fields = 4;
		size = nut->ft[i].lsb;

		for (count = 0; i < 256; count++, i++) {
			if (i == 'N') { count--; continue; }
			if (nut->ft[i].flags != flag) break;
			if (nut->ft[i].stream_plus1 != stream) break;
			if (nut->ft[i].mul != mul) break;
			if (nut->ft[i].lsb != size + count) break;
			if (nut->ft[i].pts_delta != timestamp) break;
		}
		if (count != mul - size) fields = 6;

		put_v(tmp, flag);
		put_v(tmp, fields);
		if (fields > 0) put_s(tmp, timestamp);
		if (fields > 1) put_v(tmp, mul);
		if (fields > 2) put_v(tmp, stream);
		if (fields > 3) put_v(tmp, size);
		if (fields > 4) put_v(tmp, 0); // reserved
		if (fields > 5) put_v(tmp, count);
	}

	put_header(nut->o, tmp, nut->tmp_buffer2, MAIN_STARTCODE, 0);
}

static void put_stream_header(nut_context_t * nut, int id) {
	output_buffer_t * tmp = clear_buffer(nut->tmp_buffer);
	stream_context_t * sc = &nut->sc[id];

	put_v(tmp, id); // ### is stream_id staying in spec
	put_v(tmp, sc->sh.type);
	put_vb(tmp, sc->sh.fourcc_len, sc->sh.fourcc);
	put_v(tmp, sc->sh.timebase.nom);
	put_v(tmp, sc->sh.timebase.den);
	put_v(tmp, sc->msb_pts_shift);
	put_v(tmp, sc->max_pts_distance);
	put_v(tmp, sc->sh.decode_delay);
	put_bytes(tmp, 1, sc->sh.fixed_fps ? 1 : 0);
	put_vb(tmp, sc->sh.codec_specific_len, sc->sh.codec_specific);

	switch (sc->sh.type) {
		case NUT_VIDEO_CLASS:
			put_v(tmp, sc->sh.width);
			put_v(tmp, sc->sh.height);
			put_v(tmp, sc->sh.sample_width);
			put_v(tmp, sc->sh.sample_height);
			put_v(tmp, sc->sh.colorspace_type); // TODO understand this
			break;
		case NUT_AUDIO_CLASS:
			put_v(tmp, sc->sh.samplerate_nom);
			put_v(tmp, sc->sh.samplerate_denom);
			put_v(tmp, sc->sh.channel_count); // ### is channel count staying in spec
			break;
	}

	put_header(nut->o, tmp, nut->tmp_buffer2, STREAM_STARTCODE, 0);
}

static void put_info(nut_context_t * nut, nut_info_packet_t * info) {
	output_buffer_t * tmp = clear_buffer(nut->tmp_buffer);
	int i;

	put_v(tmp, info->stream_id_plus1);
	put_v(tmp, info->chapter_id);
	put_v(tmp, info->chapter_start);
	put_v(tmp, info->chapter_len);
	put_v(tmp, info->count);

	for(i = 0; i < info->count; i++){
		nut_info_field_t * field = &info->fields[i];
		put_vb(tmp, strlen(field->name), field->name);
		if (!strcmp(field->type, "v")) {
			put_s(tmp, field->val);
		} else if (!strcmp(field->type, "s")) {
			put_s(tmp, -3);
			put_s(tmp, field->val);
		} else if (!strcmp(field->type, "t")) {
			put_s(tmp, -4);
			put_v(tmp, field->val);
		} else if (!strcmp(field->type, "r")) {
			put_s(tmp, -(field->den + 4));
			put_s(tmp, field->val);
		} else {
			if (strcmp(field->type, "UTF-8")) {
				put_s(tmp, -1);
			} else {
				put_s(tmp, -2);
				put_vb(tmp, strlen(field->type), field->type);
			}
			put_vb(tmp, field->val, field->data);
		}
	}

	put_header(nut->o, tmp, nut->tmp_buffer2, INFO_STARTCODE, 0);
}

static void put_headers(nut_context_t * nut) {
	int i;
	nut->last_headers = bctello(nut->o);

	put_main_header(nut);
	for (i = 0; i < nut->stream_count; i++) put_stream_header(nut, i);
	for (i = 0; i < nut->info_count; i++) put_info(nut, &nut->info[i]);
}

static void put_syncpoint(nut_context_t * nut) {
	output_buffer_t * tmp = clear_buffer(nut->tmp_buffer);
	int i;
	uint64_t pts = 0;
	int stream = 0;
	int back_ptr = 0;
	int keys[nut->stream_count];
	syncpoint_list_t * s = &nut->syncpoints;

	nut->last_syncpoint = bctello(nut->o);

	for (i = 0; i < nut->stream_count; i++) {
		if (nut->sc[i].last_dts > 0 && compare_ts(nut, nut->sc[i].last_dts, i, pts, stream) > 0) {
			pts = nut->sc[i].last_dts;
			stream = i;
		}
	}

	if (s->alloc_len <= s->len) {
		s->alloc_len += PREALLOC_SIZE;
		s->s = realloc(s->s, s->alloc_len * sizeof(syncpoint_t));
		s->pts = realloc(s->pts, s->alloc_len * nut->stream_count * sizeof(uint64_t));
		s->eor = realloc(s->eor, s->alloc_len * nut->stream_count * sizeof(uint64_t));
	}

	for (i = 0; i < nut->stream_count; i++) {
		s->pts[s->len * nut->stream_count + i] = nut->sc[i].last_key;
		s->eor[s->len * nut->stream_count + i] = nut->sc[i].eor > 0 ? nut->sc[i].eor : 0;
	}
	s->s[s->len].pos = nut->last_syncpoint;
	s->len++;

	for (i = 0; i < nut->stream_count; i++) keys[i] = !!nut->sc[i].eor;
	for (i = s->len; --i; ) {
		int j;
		int n = 1;
		for (j = 0; j < nut->stream_count; j++) {
			if (keys[j]) continue;
			if (!s->pts[i * nut->stream_count + j]) continue;
			if (compare_ts(nut, s->pts[i * nut->stream_count + j] - 1, j, pts, stream) <= 0) keys[j] = 1;
		}
		for (j = 0; j < nut->stream_count; j++) n &= keys[j];
		if (n) { i--; break; }
	}
	back_ptr = (nut->last_syncpoint - s->s[i].pos) / 16;

	for (i = 0; i < nut->stream_count; i++) {
		nut->sc[i].last_pts = convert_ts(nut, pts, stream, i);
		nut->sc[i].last_key = 0;
		if (nut->sc[i].eor) nut->sc[i].eor = -1; // so we know to ignore this stream in future syncpoints
	}

	put_v(tmp, pts * nut->stream_count + stream);
	put_v(tmp, back_ptr);

	put_header(nut->o, tmp, nut->tmp_buffer2, SYNCPOINT_STARTCODE, 0);

	nut->sync_overhead += bctello(tmp) + bctello(nut->tmp_buffer2);
}

static void put_index(nut_context_t * nut) {
	output_buffer_t * tmp = clear_buffer(nut->tmp_buffer);
	syncpoint_list_t * s = &nut->syncpoints;
	int i;
	uint64_t max_pts = 0;
	int stream = 0;

	for (i = 0; i < nut->stream_count; i++) {
		if (compare_ts(nut, nut->sc[i].sh.max_pts, i, max_pts, stream) > 0) {
			max_pts = nut->sc[i].sh.max_pts;
			stream = i;
		}
	}
	put_v(tmp, max_pts * nut->stream_count + stream);

	put_v(tmp, s->len);
	for (i = 0; i < s->len; i++) {
		off_t pos = s->s[i].pos / 16;
		off_t last_pos = i ? s->s[i-1].pos / 16 : 0;
		put_v(tmp, pos - last_pos);
	}

	for (i = 0; i < nut->stream_count; i++) {
		uint64_t a, last = 0; // all of pts[] array is off by one. using 0 for last pts is equivalent to -1 in spec.
		int j;
		for (j = 0; j < s->len; ) {
			int k;
			a = 0;
			for (k = 0; k < 5 && j+k < s->len; k++) a |= !!s->pts[(j + k) * nut->stream_count + i] << k;
			if (a == 0 || a == ((1 << k) - 1)) {
				int flag = a & 2;
				for (k = 0; j+k < s->len; k++) if (!s->pts[(j+k) * nut->stream_count + i] != !flag) break;
				put_v(tmp, k << 2 | flag | 1);
				if (j+k < s->len) k++;
			} else {
				for (; k+7 < 62 && j+k < s->len; ) {
					uint64_t b = 0;
					int tmp2;
					for (tmp2 = 0; tmp2 < 7 && j+k+tmp2 < s->len; tmp2++) {
						b |= !!s->pts[(j + k + tmp2) * nut->stream_count + i] << tmp2;
					}
					if (b == 0 || b == ((1 << tmp2) - 1)) break;
					a |= b << k;
					k += tmp2;
				}
				put_v(tmp, ((1LL << k) | a) << 1);
			}
			assert(k > 4 || j+k == s->len);
			j += k;
			for (k = j - k; k < j; k++) {
				if (!s->pts[k * nut->stream_count + i]) continue;
				if (s->eor[k * nut->stream_count + i]) {
					put_v(tmp, 0);
					put_v(tmp, s->pts[k * nut->stream_count + i] - last);
					put_v(tmp, s->eor[k * nut->stream_count + i] - s->pts[k * nut->stream_count + i]);
					last = s->eor[k * nut->stream_count + i];
				} else {
					put_v(tmp, s->pts[k * nut->stream_count + i] - last);
					last = s->pts[k * nut->stream_count + i];
				}
			}
		}
	}

	put_header(nut->o, tmp, nut->tmp_buffer2, INDEX_STARTCODE, 1);
}

static int frame_header(nut_context_t * nut, output_buffer_t * tmp, const nut_packet_t * fd) {
	stream_context_t * sc = &nut->sc[fd->stream];
	int i, ftnum = -1, size = 0, coded_pts, coded_flags = 0, msb_pts = (1 << sc->msb_pts_shift);
	int checksum = 0, pts_delta = (int64_t)fd->pts - (int64_t)sc->last_pts;

	if (ABS(pts_delta) < (msb_pts/2) - 1) coded_pts = fd->pts & (msb_pts - 1);
	else coded_pts = fd->pts + msb_pts;

	if (fd->len > nut->max_distance) checksum = 1;
	if (ABS(pts_delta) > sc->max_pts_distance) {
		fprintf(stderr, "%d > %d || %d - %d > %d   \n", fd->len, nut->max_distance, (int)fd->pts, (int)sc->last_pts, sc->max_pts_distance);
		checksum = 1;
	}

	for (i = 0; i < 256; i++) {
		int len = 1; // frame code
		int flags = nut->ft[i].flags;
		if (flags & FLAG_INVALID) continue;
		if (nut->ft[i].stream_plus1 && nut->ft[i].stream_plus1 - 1 != fd->stream) continue;
		if (nut->ft[i].pts_delta && nut->ft[i].pts_delta != pts_delta) continue;
		if (flags & FLAG_CODED) {
			flags = fd->flags & NUT_API_FLAGS;
			if (nut->ft[i].lsb != fd->len) flags |= FLAG_SIZE_MSB;
			if (checksum) flags |= FLAG_CHECKSUM;
			flags |= FLAG_CODED;
		}
		if (flags & FLAG_SIZE_MSB) { if ((fd->len - nut->ft[i].lsb) % nut->ft[i].mul) continue; }
		else { if (nut->ft[i].lsb != fd->len) continue; }
		if ((flags ^ fd->flags) & NUT_API_FLAGS) continue;
		if (checksum && !(flags & FLAG_CHECKSUM)) continue;

		len += nut->ft[i].stream_plus1  ? 0 : v_len(fd->stream);
		len += nut->ft[i].pts_delta     ? 0 : v_len(coded_pts);
		len += !(flags & FLAG_CODED)    ? 0 : v_len(flags ^ nut->ft[i].flags);
		len += !(flags & FLAG_SIZE_MSB) ? 0 : v_len((fd->len - nut->ft[i].lsb) / nut->ft[i].mul);
		len += !(flags & FLAG_CHECKSUM) ? 0 : 4;
		if (!size || len < size) { ftnum = i; coded_flags = flags; size = len; }
	}
	assert(ftnum != -1);
	if (tmp) {
		put_bytes(tmp, 1, ftnum); // frame_code
		if (!nut->ft[ftnum].stream_plus1) put_v(tmp, fd->stream);
		if (!nut->ft[ftnum].pts_delta)    put_v(tmp, coded_pts);
		if (coded_flags & FLAG_CODED)     put_v(tmp, coded_flags ^ nut->ft[ftnum].flags);
		if (coded_flags & FLAG_SIZE_MSB)  put_v(tmp, (fd->len - nut->ft[ftnum].lsb) / nut->ft[ftnum].mul);
		if (coded_flags & FLAG_CHECKSUM)  put_bytes(tmp, 4, crc32(tmp->buf, bctello(tmp)));
	}
	return size;
}

void nut_write_frame(nut_context_t * nut, const nut_packet_t * fd, const uint8_t * buf) {
	stream_context_t * sc = &nut->sc[fd->stream];
	output_buffer_t * tmp;
	int i;

	if (bctello(nut->o) > (1 << 23)) { // main header repetition
		i = 23; // ### magic value for header repetition
		if (bctello(nut->o) > (1 << 25)) {
			for (i = 26; bctello(nut->o) > (1 << i); i++);
			i--;
		}
		if (nut->last_headers < (1 << i)) {
			put_headers(nut);
		}
	}
	// distance syncpoints
	if (nut->last_syncpoint < nut->last_headers  ||
		bctello(nut->o) - nut->last_syncpoint + fd->len + frame_header(nut, NULL, fd) > nut->max_distance) put_syncpoint(nut);

	tmp = clear_buffer(nut->tmp_buffer);
	sc->overhead += frame_header(nut, tmp, fd);
	sc->total_frames++;
	sc->tot_size += fd->len;

	put_data(nut->o, bctello(tmp), tmp->buf);
	put_data(nut->o, fd->len, buf);

        for (i = 0; i < nut->stream_count; i++) {
		if (nut->sc[i].last_dts == -1) continue;
		if (compare_ts(nut, fd->pts, fd->stream, nut->sc[i].last_dts, i) < 0)
			fprintf(stderr, "%lld %d (%f) %lld %d (%f) \n",
				fd->pts, fd->stream, TO_DOUBLE(fd->stream, fd->pts),
				nut->sc[i].last_dts, i, TO_DOUBLE(i, nut->sc[i].last_dts));
		assert(compare_ts(nut, fd->pts, fd->stream, nut->sc[i].last_dts, i) >= 0);
	}

	sc->last_pts = fd->pts;
	sc->last_dts = get_dts(sc->sh.decode_delay, sc->pts_cache, fd->pts);
	sc->sh.max_pts = MAX(sc->sh.max_pts, fd->pts);

	if ((fd->flags & NUT_FLAG_KEY) && !sc->last_key) sc->last_key = fd->pts + 1;
	if (fd->flags & NUT_FLAG_EOR) sc->eor = fd->pts + 1;
	else sc->eor = 0;
}

nut_context_t * nut_muxer_init(const nut_muxer_opts_t * mopts, const nut_stream_header_t s[], const nut_info_packet_t info[]) {
	nut_context_t * nut = malloc(sizeof(nut_context_t));
	int i;
	// TODO check that all input is valid

	nut->o = new_output_buffer(mopts->output);
	nut->tmp_buffer = new_mem_buffer(); // general purpose buffer
	nut->tmp_buffer2 = new_mem_buffer(); //  for packet_headers
	nut->max_distance = mopts->max_distance;
	nut->mopts = *mopts;

	if (nut->max_distance > 131072) nut->max_distance = 131072;

	{
	int j, n;
	int flag, fields, timestamp = 0, mul = 1, stream = 0, size, count;

	for(n=i=0; i < 256; n++) {
		assert(mopts->fti[n].tmp_flag != -1);
		flag   = mopts->fti[n].tmp_flag;
		fields = mopts->fti[n].tmp_fields;
		if (fields > 0) timestamp = mopts->fti[n].tmp_pts;
		if (fields > 1) mul       = mopts->fti[n].tmp_mul;
		if (fields > 2) stream    = mopts->fti[n].tmp_stream;
		if (fields > 3) size      = mopts->fti[n].tmp_size;
		else size = 0;
		if (fields > 5) count     = mopts->fti[n].count;
		else count = mul - size;

		for(j = 0; j < count && i < 256; j++, i++) {
			if (i == 'N') {
				nut->ft[i].flags = INVALID_FLAG;
				j--;
				continue;
			}
			nut->ft[i].flags = flag;
			nut->ft[i].stream_plus1 = stream;
			nut->ft[i].mul = mul;
			nut->ft[i].lsb = size + j;
			nut->ft[i].pts_delta = timestamp;
		}
	}
	assert(mopts->fti[n].tmp_flag == -1);
	}

	nut->sync_overhead = 0;

	nut->syncpoints.len = 0;
	nut->syncpoints.alloc_len = 0;
	nut->syncpoints.s = NULL;
	nut->syncpoints.pts = NULL;
	nut->syncpoints.eor = NULL;
	nut->last_syncpoint = 0;

	for (nut->stream_count = 0; s[nut->stream_count].type >= 0; nut->stream_count++);

	nut->sc = malloc(sizeof(stream_context_t) * nut->stream_count);

	for (i = 0; i < nut->stream_count; i++) {
		int j;
		nut->sc[i].last_key = 0;
		nut->sc[i].last_pts = 0;
		nut->sc[i].last_dts = -1;
		nut->sc[i].msb_pts_shift = 7; // TODO
		nut->sc[i].max_pts_distance = (s[i].timebase.den + s[i].timebase.nom - 1) / s[i].timebase.nom; // TODO
		nut->sc[i].eor = 0;
		nut->sc[i].sh = s[i];
		nut->sc[i].sh.max_pts = 0;

		nut->sc[i].sh.fourcc = malloc(s[i].fourcc_len);
		memcpy(nut->sc[i].sh.fourcc, s[i].fourcc, s[i].fourcc_len);

		nut->sc[i].sh.codec_specific = malloc(s[i].codec_specific_len);
		memcpy(nut->sc[i].sh.codec_specific, s[i].codec_specific, s[i].codec_specific_len);

		nut->sc[i].pts_cache = malloc(nut->sc[i].sh.decode_delay * sizeof(int64_t));

		// reorder.c
		nut->sc[i].reorder_pts_cache = malloc(nut->sc[i].sh.decode_delay * sizeof(int64_t));
		for (j = 0; j < nut->sc[i].sh.decode_delay; j++) nut->sc[i].reorder_pts_cache[j] = nut->sc[i].pts_cache[j] = -1;
		nut->sc[i].next_pts = 0;
		nut->sc[i].packets = NULL;
		nut->sc[i].num_packets = 0;

		// debug
		nut->sc[i].total_frames = 0;
		nut->sc[i].overhead = 0;
		nut->sc[i].tot_size = 0;
	}

	if (info) {
		for (nut->info_count = 0; info[nut->info_count].count >= 0; nut->info_count++);

		nut->info = malloc(sizeof(nut_info_packet_t) * nut->info_count);

		for (i = 0; i < nut->info_count; i++) {
			int j;
			nut->info[i] = info[i];
			nut->info[i].fields = malloc(sizeof(nut_info_field_t) * info[i].count);
			for (j = 0; j < info[i].count; j++) {
				nut->info[i].fields[j] = info[i].fields[j];
				if (info[i].fields[j].data) {
					nut->info[i].fields[j].data = malloc(info[i].fields[j].val);
					memcpy(nut->info[i].fields[j].data, info[i].fields[j].data, info[i].fields[j].val);
				}
			}
		}
	} else {
		nut->info_count = 0;
		nut->info = NULL;
	}

	put_data(nut->o, strlen(ID_STRING) + 1, ID_STRING);

	put_headers(nut);

	return nut;
}

void nut_muxer_uninit(nut_context_t * nut) {
	int i;
	int total = 0;
	if (!nut) return;

	put_headers(nut);
	if (nut->mopts.write_index) put_index(nut);

	for (i = 0; i < nut->stream_count; i++) {
		total += nut->sc[i].tot_size;
		fprintf(stderr, "Stream %d:\n", i);
		fprintf(stderr, "   frames: %d\n", nut->sc[i].total_frames);
		fprintf(stderr, "   TOT: ");
		fprintf(stderr, "packet size: %d ", nut->sc[i].tot_size);
		fprintf(stderr, "packet overhead: %d ", nut->sc[i].overhead);
		fprintf(stderr, "(%.2lf%%)\n", (double)nut->sc[i].overhead / nut->sc[i].tot_size * 100);
		fprintf(stderr, "   AVG: ");
		fprintf(stderr, "packet size: %.2lf ", (double)nut->sc[i].tot_size / nut->sc[i].total_frames);
		fprintf(stderr, "packet overhead: %.2lf\n", (double)nut->sc[i].overhead / nut->sc[i].total_frames);

		free(nut->sc[i].sh.fourcc);
		free(nut->sc[i].sh.codec_specific);
		free(nut->sc[i].pts_cache);
		free(nut->sc[i].reorder_pts_cache);
	}
	free(nut->sc);

	for (i = 0; i < nut->info_count; i++) {
		int j;
		for (j = 0; j < nut->info[i].count; j++) free(nut->info[i].fields[j].data);
		free(nut->info[i].fields);
	}
	free(nut->info);

	fprintf(stderr, "Syncpoints: %d size: %d\n", nut->syncpoints.len, nut->sync_overhead);

	free(nut->syncpoints.s);
	free(nut->syncpoints.pts);
	free(nut->syncpoints.eor);

	free_buffer(nut->tmp_buffer);
	free_buffer(nut->tmp_buffer2);
	free_buffer(nut->o); // flushes file
	fprintf(stderr, "TOTAL: %d bytes data, %d bytes overhead, %.2lf%% overhead\n", total,
		(int)ftell(nut->mopts.output.priv) - total, (double)(ftell(nut->mopts.output.priv) - total) / total*100);
	free(nut);
}
