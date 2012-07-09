// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/mp4/aac.h"

#include "base/logging.h"
#include "media/base/bit_reader.h"
#include "media/mp4/rcheck.h"

// The following conversion table is extracted from ISO 14496 Part 3 -
// Table 1.16 - Sampling Frequency Index.
static const uint32 kFrequencyMap[] = {
  96000, 88200, 64000, 48000, 44100, 32000, 24000,
  22050, 16000, 12000, 11025, 8000, 7350
};

static ChannelLayout GetChannelLayout(uint8 channel_config) {
  switch (channel_config) {
    case 1:
      return CHANNEL_LAYOUT_MONO;
    case 2:
      return CHANNEL_LAYOUT_STEREO;
    case 3:
      return CHANNEL_LAYOUT_SURROUND;
    case 4:
      return CHANNEL_LAYOUT_4_0;
    case 5:
      return CHANNEL_LAYOUT_5_0;
    case 6:
      return CHANNEL_LAYOUT_5_1;
    case 8:
      return CHANNEL_LAYOUT_7_1;
    default:
      break;
  }

  return CHANNEL_LAYOUT_UNSUPPORTED;
}

namespace media {

namespace mp4 {

AAC::AAC()
    : profile_(0), frequency_index_(0), channel_config_(0), frequency_(0),
      channel_layout_(CHANNEL_LAYOUT_UNSUPPORTED) {
}

AAC::~AAC() {
}

bool AAC::Parse(const std::vector<uint8>& data) {
  BitReader reader;
  uint8 extension_type = 0;
  bool ps_present = false;
  uint8 extension_frequency_index;

  profile_ = 0;
  frequency_index_ = 0;
  frequency_ = 0;
  channel_config_ = 0;

  reader.Initialize(&data[0], data.size());

  // The following code is written according to ISO 14496 Part 3 Table 1.13 -
  // Syntax of AudioSpecificConfig.

  // Read base configuration
  RCHECK(reader.ReadBits(5, &profile_));
  RCHECK(reader.ReadBits(4, &frequency_index_));
  if (frequency_index_ == 0xf)
    RCHECK(reader.ReadBits(24, &frequency_));
  RCHECK(reader.ReadBits(4, &channel_config_));

  extension_frequency_index = frequency_index_;

  // Read extension configuration
  if (profile_ == 5 || profile_ == 29) {
    ps_present = (profile_ == 29);
    extension_type = 5;
    RCHECK(reader.ReadBits(4, &extension_frequency_index));
    if (extension_frequency_index == 0xf)
      RCHECK(reader.ReadBits(24, &frequency_));
    RCHECK(reader.ReadBits(5, &profile_));
  }

  RCHECK(SkipDecoderGASpecificConfig(&reader));
  RCHECK(SkipErrorSpecificConfig());

  // Read extension configuration again
  if (extension_type != 5 && reader.NumBitsLeft() >= 16) {
    uint16 sync_extension_type;
    uint8 sbr_present_flag;
    uint8 ps_present_flag;

    RCHECK(reader.ReadBits(11, &sync_extension_type));

    if (sync_extension_type == 0x2b7) {
      RCHECK(reader.ReadBits(5, &extension_type));

      if (extension_type == 5) {
        RCHECK(reader.ReadBits(1, &sbr_present_flag));

        if (sbr_present_flag) {
          RCHECK(reader.ReadBits(4, &extension_frequency_index));

          if (extension_frequency_index == 0xf)
            RCHECK(reader.ReadBits(24, &frequency_));

          RCHECK(reader.ReadBits(11, &sync_extension_type));

          if (sync_extension_type == 0x548) {
            RCHECK(reader.ReadBits(1, &ps_present_flag));
            ps_present = ps_present_flag != 0;
          }
        }
      }
    }
  }

  if (frequency_ == 0) {
    RCHECK(extension_frequency_index < arraysize(kFrequencyMap));
    frequency_ = kFrequencyMap[extension_frequency_index];
  }

  // When Parametric Stereo is on, mono will be played as stereo.
  if (ps_present && channel_config_ == 1)
    channel_layout_ = GetChannelLayout(channel_config_ + 1);
  else
    channel_layout_ = GetChannelLayout(channel_config_);

  return frequency_ != 0 && channel_layout_ != CHANNEL_LAYOUT_UNSUPPORTED &&
      profile_ >= 1 && profile_ <= 4 && frequency_index_ != 0xf &&
      channel_config_ <= 7;
}

uint32 AAC::frequency() const {
  return frequency_;
}

ChannelLayout AAC::channel_layout() const {
  return channel_layout_;
}

bool AAC::ConvertEsdsToADTS(std::vector<uint8>* buffer) const {
  size_t size = buffer->size() + 7;

  DCHECK(profile_ >= 1 && profile_ <= 4 && frequency_index_ != 0xf &&
         channel_config_ <= 7);

  // ADTS header uses 13 bits for packet size.
  if (size > (1 << 13) - 1)
    return false;

  std::vector<uint8>& adts = *buffer;

  adts.insert(buffer->begin(), 7, 0);
  adts[0] = 0xff;
  adts[1] = 0xf1;
  adts[2] = ((profile_ - 1) << 6) + (frequency_index_ << 2) +
      (channel_config_ >> 2);
  adts[3] = ((channel_config_ & 0x3) << 6) + (size >> 11);
  adts[4] = (size & 0x7ff) >> 3;
  adts[5] = ((size & 7) << 5) + 0x1f;
  adts[6] = 0xfc;

  return true;
}

// Currently this function only support GASpecificConfig defined in
// ISO 14496 Part 3 Table 4.1 - Syntax of GASpecificConfig()
bool AAC::SkipDecoderGASpecificConfig(BitReader* bit_reader) const {
  switch (profile_) {
    case 1:
    case 2:
    case 3:
    case 4:
    case 6:
    case 7:
    case 17:
    case 19:
    case 20:
    case 21:
    case 22:
    case 23:
      return SkipGASpecificConfig(bit_reader);
    default:
      break;
  }

  return false;
}

bool AAC::SkipErrorSpecificConfig() const {
  switch (profile_) {
    case 17:
    case 19:
    case 20:
    case 21:
    case 22:
    case 23:
    case 24:
    case 25:
    case 26:
    case 27:
      return false;
    default:
      break;
  }

  return true;
}

// The following code is written according to ISO 14496 part 3 Table 4.1 -
// GASpecificConfig.
bool AAC::SkipGASpecificConfig(BitReader* bit_reader) const {
  uint8 extension_flag = 0;
  uint8 depends_on_core_coder;

  RCHECK(bit_reader->SkipBits(1));  // frameLengthFlag
  RCHECK(bit_reader->ReadBits(1, &depends_on_core_coder));
  if (depends_on_core_coder == 1)
    RCHECK(bit_reader->SkipBits(14));  // coreCoderDelay

  RCHECK(bit_reader->ReadBits(1, &extension_flag));
  RCHECK(channel_config_ != 0);

  if (profile_ == 6 || profile_ == 20)
    RCHECK(bit_reader->SkipBits(3));

  if (extension_flag) {
    if (profile_ == 22) {
      RCHECK(bit_reader->SkipBits(5));
      RCHECK(bit_reader->SkipBits(11));
    }

    if (profile_ == 17 || profile_ == 19 || profile_ == 20 || profile_ == 23) {
      RCHECK(bit_reader->SkipBits(1));
      RCHECK(bit_reader->SkipBits(1));
      RCHECK(bit_reader->SkipBits(1));
    }

    RCHECK(bit_reader->SkipBits(1));
  }

  return true;
}

}  // namespace mp4

}  // namespace media
