// Copyright 2012 The Cobalt Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef MEDIA_STARBOARD_PROGRESSIVE_MP4_PARSER_H_
#define MEDIA_STARBOARD_PROGRESSIVE_MP4_PARSER_H_

#include "media/base/media_log.h"
#include "media/starboard/progressive/avc_parser.h"
#include "media/starboard/progressive/mp4_map.h"

namespace media {

// How many bytes to download from the start of the atom? Should be large
// enough that we can extract all the data we need from the atom without
// second download (typically), but no larger. This is currently set at 16
// bytes for the 8 byte header + optional 8 byte size extension plus 20 bytes
// for the needed values within an mvhd header. We leave this is the header so
// that MP4Map can reuse,
static const int kAtomDownload = 36;

// mp4 atom fourCC codes as big-endian unsigned ints
static const uint32_t kAtomType_avc1 = 0x61766331;  // skip in to subatom
static const uint32_t kAtomType_avcC = 0x61766343;  // download and parse
static const uint32_t kAtomType_co64 = 0x636f3634;  // cache in table
static const uint32_t kAtomType_ctts = 0x63747473;  // cache in table
static const uint32_t kAtomType_dinf = 0x64696e66;  // skip whole atom
static const uint32_t kAtomType_dref = 0x64726566;  // skip whole atom
static const uint32_t kAtomType_esds = 0x65736473;  // download and parse
static const uint32_t kAtomType_ftyp = 0x66747970;  // top of the file only
static const uint32_t kAtomType_hdlr = 0x68646c72;  // parse first 12 bytes
static const uint32_t kAtomType_mdhd = 0x6d646864;  // parse first 20 bytes
static const uint32_t kAtomType_mdia = 0x6d646961;  // container atom, no-op
static const uint32_t kAtomType_minf = 0x6d696e66;  // container atom, no-op
static const uint32_t kAtomType_moov = 0x6d6f6f76;  // container atom, no-op
static const uint32_t kAtomType_mp4a = 0x6d703461;  // parse first 10 bytes
static const uint32_t kAtomType_mvhd = 0x6d766864;  // parse first 20 bytes
static const uint32_t kAtomType_smhd = 0x736d6862;  // skip whole atom
static const uint32_t kAtomType_stbl = 0x7374626c;  // container atom, no-op
static const uint32_t kAtomType_stco = 0x7374636f;  // cache in table
static const uint32_t kAtomType_stts = 0x73747473;  // cache in table
static const uint32_t kAtomType_stsc = 0x73747363;  // cache in table
static const uint32_t kAtomType_stsd = 0x73747364;  // skip in to subatom
static const uint32_t kAtomType_stss = 0x73747373;  // cache in table
static const uint32_t kAtomType_stsz = 0x7374737a;  // cache in table
static const uint32_t kAtomType_trak = 0x7472616b;  // container atom, no-op
static const uint32_t kAtomType_tkhd = 0x746b6864;  // skip whole atom
static const uint32_t kAtomType_vmhd = 0x766d6864;  // skip whole atom
// TODO: mp4v!!

class MP4Parser : public AVCParser {
 public:
  // Attempts to make sense of the provided bytes of the top of a file as an
  // flv, and if it does make sense returns PIPELINE_OK and |*parser| contains a
  // MP4Parser initialized with some basic state.  If it doesn't make sense
  // this returns an error status and |*parser| contains NULL.
  static ::media::PipelineStatus Construct(
      scoped_refptr<DataSourceReader> reader,
      const uint8_t* construction_header,
      scoped_refptr<ProgressiveParser>* parser,
      MediaLog* media_log);
  MP4Parser(scoped_refptr<DataSourceReader> reader,
            uint32_t ftyp_atom_size,
            MediaLog* media_log);
  ~MP4Parser() override;

  // === ProgressiveParser implementation
  bool ParseConfig() override;
  scoped_refptr<AvcAccessUnit> GetNextAU(DemuxerStream::Type type) override;
  bool SeekTo(base::TimeDelta timestamp) override;

 private:
  bool ParseNextAtom();
  bool ParseMP4_esds(uint64_t atom_data_size);
  bool ParseMP4_hdlr(uint64_t atom_data_size, uint8_t* hdlr);
  bool ParseMP4_mdhd(uint64_t atom_data_size, uint8_t* mdhd);
  bool ParseMP4_mp4a(uint64_t atom_data_size, uint8_t* mp4a);
  bool ParseMP4_mvhd(uint64_t atom_data_size, uint8_t* mvhd);
  base::TimeDelta TicksToTime(uint64_t ticks, uint32_t time_scale_hz);
  uint64_t TimeToTicks(base::TimeDelta time, uint32_t time_scale_hz);

  uint64_t atom_offset_;
  bool current_trak_is_video_;
  bool current_trak_is_audio_;
  uint32_t current_trak_time_scale_;
  base::TimeDelta current_trak_duration_;
  uint32_t video_time_scale_hz_;
  base::TimeDelta one_video_tick_;
  uint32_t audio_time_scale_hz_;
  base::TimeDelta audio_track_duration_;
  base::TimeDelta video_track_duration_;
  scoped_refptr<MP4Map> audio_map_;
  scoped_refptr<MP4Map> video_map_;
  uint32_t audio_sample_;
  uint32_t video_sample_;
  // for keeping buffers continuous across time scales
  uint64_t first_audio_hole_ticks_;
  base::TimeDelta first_audio_hole_;
};

}  // namespace media

#endif  // MEDIA_STARBOARD_PROGRESSIVE_MP4_PARSER_H_
