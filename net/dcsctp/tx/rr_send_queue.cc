/*
 *  Copyright (c) 2021 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */
#include "net/dcsctp/tx/rr_send_queue.h"

#include <cstdint>
#include <deque>
#include <map>
#include <utility>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/types/optional.h"
#include "api/array_view.h"
#include "net/dcsctp/packet/data.h"
#include "net/dcsctp/public/dcsctp_message.h"
#include "net/dcsctp/public/dcsctp_socket.h"
#include "net/dcsctp/public/types.h"
#include "net/dcsctp/tx/send_queue.h"
#include "rtc_base/logging.h"

namespace dcsctp {

RRSendQueue::OutgoingStream::Item*
RRSendQueue::OutgoingStream::GetFirstNonExpiredMessage(TimeMs now) {
  while (!items_.empty()) {
    RRSendQueue::OutgoingStream::Item& item = items_.front();
    // An entire item can be discarded iff:
    // 1) It hasn't been partially sent (has been allocated a message_id).
    // 2) It has a non-negative expiry time.
    // 3) And that expiry time has passed.
    if (!item.message_id.has_value() && item.expires_at.has_value() &&
        *item.expires_at <= now) {
      // TODO(boivie): This should be reported to the client.
      buffered_amount_.Decrease(item.remaining_size);
      items_.pop_front();
      continue;
    }

    RTC_DCHECK(IsConsistent());
    return &item;
  }
  RTC_DCHECK(IsConsistent());
  return nullptr;
}

bool RRSendQueue::OutgoingStream::IsConsistent() const {
  size_t bytes = 0;
  for (const auto& item : items_) {
    bytes += item.remaining_size;
  }
  return bytes == buffered_amount_.value();
}

void RRSendQueue::ThresholdWatcher::Decrease(size_t bytes) {
  RTC_DCHECK(bytes <= value_);
  size_t old_value = value_;
  value_ -= bytes;

  if (old_value > low_threshold_ && value_ <= low_threshold_) {
    on_threshold_reached_();
  }
}

void RRSendQueue::ThresholdWatcher::SetLowThreshold(size_t low_threshold) {
  // Betting on https://github.com/w3c/webrtc-pc/issues/2654 being accepted.
  if (low_threshold_ < value_ && low_threshold >= value_) {
    on_threshold_reached_();
  }
  low_threshold_ = low_threshold;
}

void RRSendQueue::OutgoingStream::Add(DcSctpMessage message,
                                      absl::optional<TimeMs> expires_at,
                                      const SendOptions& send_options) {
  buffered_amount_.Increase(message.payload().size());
  items_.emplace_back(std::move(message), expires_at, send_options);

  RTC_DCHECK(IsConsistent());
}

absl::optional<SendQueue::DataToSend> RRSendQueue::OutgoingStream::Produce(
    TimeMs now,
    size_t max_size) {
  Item* item = GetFirstNonExpiredMessage(now);
  if (item == nullptr) {
    RTC_DCHECK(IsConsistent());
    return absl::nullopt;
  }

  // If a stream is paused, it will allow sending all partially sent messages
  // but will not start sending new fragments of completely unsent messages.
  if (is_paused_ && !item->message_id.has_value()) {
    RTC_DCHECK(IsConsistent());
    return absl::nullopt;
  }

  DcSctpMessage& message = item->message;

  if (item->remaining_size > max_size && max_size < kMinimumFragmentedPayload) {
    RTC_DCHECK(IsConsistent());
    return absl::nullopt;
  }

  // Allocate Message ID and SSN when the first fragment is sent.
  if (!item->message_id.has_value()) {
    MID& mid =
        item->send_options.unordered ? next_unordered_mid_ : next_ordered_mid_;
    item->message_id = mid;
    mid = MID(*mid + 1);
  }
  if (!item->send_options.unordered && !item->ssn.has_value()) {
    item->ssn = next_ssn_;
    next_ssn_ = SSN(*next_ssn_ + 1);
  }

  // Grab the next `max_size` fragment from this message and calculate flags.
  rtc::ArrayView<const uint8_t> chunk_payload =
      item->message.payload().subview(item->remaining_offset, max_size);
  rtc::ArrayView<const uint8_t> message_payload = message.payload();
  Data::IsBeginning is_beginning(chunk_payload.data() ==
                                 message_payload.data());
  Data::IsEnd is_end((chunk_payload.data() + chunk_payload.size()) ==
                     (message_payload.data() + message_payload.size()));

  StreamID stream_id = message.stream_id();
  PPID ppid = message.ppid();

  // Zero-copy the payload if the message fits in a single chunk.
  std::vector<uint8_t> payload =
      is_beginning && is_end
          ? std::move(message).ReleasePayload()
          : std::vector<uint8_t>(chunk_payload.begin(), chunk_payload.end());

  FSN fsn(item->current_fsn);
  item->current_fsn = FSN(*item->current_fsn + 1);
  buffered_amount_.Decrease(payload.size());

  SendQueue::DataToSend chunk(Data(stream_id, item->ssn.value_or(SSN(0)),
                                   item->message_id.value(), fsn, ppid,
                                   std::move(payload), is_beginning, is_end,
                                   item->send_options.unordered));
  chunk.max_retransmissions = item->send_options.max_retransmissions;
  chunk.expires_at = item->expires_at;

  if (is_end) {
    // The entire message has been sent, and its last data copied to `chunk`, so
    // it can safely be discarded.
    items_.pop_front();
  } else {
    item->remaining_offset += chunk_payload.size();
    item->remaining_size -= chunk_payload.size();
    RTC_DCHECK(item->remaining_offset + item->remaining_size ==
               item->message.payload().size());
    RTC_DCHECK(item->remaining_size > 0);
  }
  RTC_DCHECK(IsConsistent());
  return chunk;
}

bool RRSendQueue::OutgoingStream::Discard(IsUnordered unordered,
                                          MID message_id) {
  bool result = false;
  if (!items_.empty()) {
    Item& item = items_.front();
    if (item.send_options.unordered == unordered &&
        item.message_id.has_value() && *item.message_id == message_id) {
      buffered_amount_.Decrease(item.remaining_size);
      items_.pop_front();
      // As the item still existed, it had unsent data.
      result = true;
    }
  }
  RTC_DCHECK(IsConsistent());
  return result;
}

void RRSendQueue::OutgoingStream::Pause() {
  is_paused_ = true;

  // A stream is paused when it's about to be reset. In this implementation,
  // it will throw away all non-partially send messages. This is subject to
  // change. It will however not discard any partially sent messages - only
  // whole messages. Partially delivered messages (at the time of receiving a
  // Stream Reset command) will always deliver all the fragments before
  // actually resetting the stream.
  for (auto it = items_.begin(); it != items_.end();) {
    if (it->remaining_offset == 0) {
      buffered_amount_.Decrease(it->remaining_size);
      it = items_.erase(it);
    } else {
      ++it;
    }
  }
  RTC_DCHECK(IsConsistent());
}

void RRSendQueue::OutgoingStream::Reset() {
  if (!items_.empty()) {
    // If this message has been partially sent, reset it so that it will be
    // re-sent.
    auto& item = items_.front();
    buffered_amount_.Increase(item.message.payload().size() -
                              item.remaining_size);
    item.remaining_offset = 0;
    item.remaining_size = item.message.payload().size();
    item.message_id = absl::nullopt;
    item.ssn = absl::nullopt;
    item.current_fsn = FSN(0);
  }
  is_paused_ = false;
  next_ordered_mid_ = MID(0);
  next_unordered_mid_ = MID(0);
  next_ssn_ = SSN(0);
  RTC_DCHECK(IsConsistent());
}

bool RRSendQueue::OutgoingStream::has_partially_sent_message() const {
  if (items_.empty()) {
    return false;
  }
  return items_.front().message_id.has_value();
}

void RRSendQueue::Add(TimeMs now,
                      DcSctpMessage message,
                      const SendOptions& send_options) {
  RTC_DCHECK(!message.payload().empty());
  // Any limited lifetime should start counting from now - when the message
  // has been added to the queue.
  absl::optional<TimeMs> expires_at = absl::nullopt;
  if (send_options.lifetime.has_value()) {
    // `expires_at` is the time when it expires. Which is slightly larger than
    // the message's lifetime, as the message is alive during its entire
    // lifetime (which may be zero).
    expires_at = now + *send_options.lifetime + DurationMs(1);
  }
  GetOrCreateStreamInfo(message.stream_id())
      .Add(std::move(message), expires_at, send_options);
}

size_t RRSendQueue::total_bytes() const {
  // TODO(boivie): Have the current size as a member variable, so that it's not
  // calculated for every operation.
  size_t bytes = 0;
  for (const auto& stream : streams_) {
    bytes += stream.second.buffered_amount().value();
  }

  return bytes;
}

bool RRSendQueue::IsFull() const {
  return total_bytes() >= buffer_size_;
}

bool RRSendQueue::IsEmpty() const {
  return total_bytes() == 0;
}

absl::optional<SendQueue::DataToSend> RRSendQueue::Produce(
    std::map<StreamID, RRSendQueue::OutgoingStream>::iterator it,
    TimeMs now,
    size_t max_size) {
  absl::optional<DataToSend> data = it->second.Produce(now, max_size);
  if (data.has_value()) {
    RTC_DLOG(LS_VERBOSE) << log_prefix_ << "tx-msg: Producing chunk of "
                         << data->data.size() << " bytes (max: " << max_size
                         << ")";

    if (data->data.is_end) {
      // No more fragments. Continue with the next stream next time.
      next_stream_id_ = StreamID(*it->first + 1);
    }
  }

  return data;
}

absl::optional<SendQueue::DataToSend> RRSendQueue::Produce(TimeMs now,
                                                           size_t max_size) {
  auto start_it = streams_.lower_bound(next_stream_id_);
  for (auto it = start_it; it != streams_.end(); ++it) {
    absl::optional<DataToSend> ret = Produce(it, now, max_size);
    if (ret.has_value()) {
      return ret;
    }
  }

  for (auto it = streams_.begin(); it != start_it; ++it) {
    absl::optional<DataToSend> ret = Produce(it, now, max_size);
    if (ret.has_value()) {
      return ret;
    }
  }
  return absl::nullopt;
}

bool RRSendQueue::Discard(IsUnordered unordered,
                          StreamID stream_id,
                          MID message_id) {
  return GetOrCreateStreamInfo(stream_id).Discard(unordered, message_id);
}

void RRSendQueue::PrepareResetStreams(rtc::ArrayView<const StreamID> streams) {
  for (StreamID stream_id : streams) {
    GetOrCreateStreamInfo(stream_id).Pause();
  }
}

bool RRSendQueue::CanResetStreams() const {
  // Streams can be reset if those streams that are paused don't have any
  // messages that are partially sent.
  for (auto& stream : streams_) {
    if (stream.second.is_paused() &&
        stream.second.has_partially_sent_message()) {
      return false;
    }
  }
  return true;
}

void RRSendQueue::CommitResetStreams() {
  Reset();
}

void RRSendQueue::RollbackResetStreams() {
  for (auto& stream_entry : streams_) {
    stream_entry.second.Resume();
  }
}

void RRSendQueue::Reset() {
  for (auto& stream_entry : streams_) {
    OutgoingStream& stream = stream_entry.second;
    stream.Reset();
  }
}

size_t RRSendQueue::buffered_amount(StreamID stream_id) const {
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) {
    return 0;
  }
  return it->second.buffered_amount().value();
}

size_t RRSendQueue::buffered_amount_low_threshold(StreamID stream_id) const {
  auto it = streams_.find(stream_id);
  if (it == streams_.end()) {
    return 0;
  }
  return it->second.buffered_amount().low_threshold();
}

void RRSendQueue::SetBufferedAmountLowThreshold(StreamID stream_id,
                                                size_t bytes) {
  GetOrCreateStreamInfo(stream_id).buffered_amount().SetLowThreshold(bytes);
}

RRSendQueue::OutgoingStream& RRSendQueue::GetOrCreateStreamInfo(
    StreamID stream_id) {
  auto it = streams_.find(stream_id);
  if (it != streams_.end()) {
    return it->second;
  }

  return streams_
      .emplace(stream_id,
               [this, stream_id]() { on_buffered_amount_low_(stream_id); })
      .first->second;
}
}  // namespace dcsctp
