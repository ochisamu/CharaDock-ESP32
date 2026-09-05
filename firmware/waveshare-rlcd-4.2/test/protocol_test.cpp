// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "charadock/frame_dispatcher.hpp"
#include "charadock/host_connection.hpp"
#include "charadock/input.hpp"
#include "charadock/monochrome_asset.hpp"
#include "charadock/presentation_protocol.hpp"
#include "charadock/protocol_v2.hpp"
#include "charadock/scene_model.hpp"
#include "charadock/shinonome_font.hpp"
#include "charadock/utf8.hpp"

using charadock::protocol::Frame;
using charadock::protocol::FrameType;
using namespace charadock::rlcd;

class FakeAudioSink final : public AudioSink {
public:
  AudioApplyResult startPlayback(uint32_t sampleRate,
                                 uint32_t totalSamples) override {
    starts += 1;
    rate = sampleRate;
    expected = totalSamples;
    active = true;
    return startResult;
  }

  AudioApplyResult writePcm16(const uint8_t *, size_t length) override {
    if (!active)
      return AudioApplyResult::InvalidState;
    bytes += length;
    return AudioApplyResult::Ok;
  }

  AudioApplyResult finishPlayback() override {
    if (!active)
      return AudioApplyResult::InvalidState;
    finishes += 1;
    return AudioApplyResult::Ok;
  }

  AudioApplyResult stopPlayback() override {
    active = false;
    stops += 1;
    return AudioApplyResult::Ok;
  }

  AudioApplyResult startResult = AudioApplyResult::Ok;
  uint32_t rate = 0;
  uint32_t expected = 0;
  size_t bytes = 0;
  int starts = 0;
  int finishes = 0;
  int stops = 0;
  bool active = false;
};

void protocolRoundTripIsFragmentSafe() {
  const Frame source{FrameType::DisplayCommit, 42, {1, 7, 0, 0, 0}};
  const auto encoded = charadock::protocol::encodeFrame(source);
  charadock::protocol::Decoder decoder;
  std::vector<Frame> frames;
  for (const uint8_t byte : encoded) {
    const auto decoded = decoder.push(&byte, 1);
    frames.insert(frames.end(), decoded.begin(), decoded.end());
  }
  assert(frames.size() == 1);
  assert(frames[0].type == FrameType::DisplayCommit);
  assert(frames[0].sequence == 42);
  assert(frames[0].payload == source.payload);
}

void utf8ValidationRejectsControlsAndMalformedText() {
  const std::string valid = "接続しました\n次の処理へ";
  assert(charadock::rlcd::utf8::validateDisplayText(
      reinterpret_cast<const uint8_t *>(valid.data()), valid.size(), 100));
  assert(charadock::rlcd::utf8::codepointCount(valid) == 12);
  const uint8_t malformed[] = {0xe3, 0x81};
  assert(!charadock::rlcd::utf8::validateDisplayText(malformed, 2, 10));
  const uint8_t control[] = {'a', 0x01, 'b'};
  assert(!charadock::rlcd::utf8::validateDisplayText(control, 3, 10));
}

std::vector<uint8_t> readFile(const char *path) {
  std::ifstream input(path, std::ios::binary);
  assert(input);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(input),
                              std::istreambuf_iterator<char>());
}

void generatedShinonomeFontsAreBoundedAndSearchable() {
  const auto font12Bytes = readFile(
      "firmware/waveshare-rlcd-4.2/src/generated/shinonome12.bin");
  const auto font16Bytes = readFile(
      "firmware/waveshare-rlcd-4.2/src/generated/shinonome16.bin");
  ShinonomeFont font12;
  ShinonomeFont font16;
  assert(font12.attach(font12Bytes.data(), font12Bytes.size()));
  assert(font16.attach(font16Bytes.data(), font16Bytes.size()));
  assert(font12.pixelSize() == 12 && font12.glyphCount() > 6900);
  assert(font16.pixelSize() == 16 && font16.glyphCount() > 6900);
  assert(font12.glyph(U'話').width == 12);
  assert(font16.glyph(U'話').width == 16);
  assert(font12.glyph(U'A').width == 6);
  assert(font16.glyph(U'A').width == 8);
  assert(font16.contains(0x25a1));
  assert(!font16.contains(0x1f680));

  auto malformed = font12Bytes;
  malformed[0] = 'X';
  ShinonomeFont rejected;
  assert(!rejected.attach(malformed.data(), malformed.size()));
}

void buttonFeedbackDebouncesAndLongPressIsInterruptible() {
  DebouncedButton button(ButtonId::Key, 400);
  assert(button.update(true, 0).empty());
  assert(button.update(false, 10).empty());
  assert(button.update(true, 20).empty());
  auto events = button.update(true, 45);
  assert(events.size() == 1 && events[0].action == ButtonAction::Pressed);
  assert(button.update(true, 444).empty());
  events = button.update(true, 445);
  assert(events.size() == 1 && events[0].action == ButtonAction::LongPress);
  assert(button.update(false, 500).empty());
  events = button.update(false, 525);
  assert(events.size() == 1 && events[0].action == ButtonAction::LongRelease);

  assert(button.update(true, 600).empty());
  events = button.update(true, 625);
  assert(events.size() == 1 && events[0].action == ButtonAction::Pressed);
  assert(button.update(false, 700).empty());
  events = button.update(false, 725);
  assert(events.size() == 1 && events[0].action == ButtonAction::ShortPress);
}

SceneSnapshot sampleScene(uint32_t revision) {
  SceneSnapshot scene;
  scene.scene = SceneId::Conversation;
  scene.state = DeviceState::Speaking;
  scene.flags = SceneConnected | SceneLive | SceneBeatrice;
  scene.revision = revision;
  scene.characterName = "コハク";
  scene.modeLabel = "Live + B2";
  return scene;
}

void scenePayloadsCommitAtomically() {
  const SceneSnapshot source = sampleScene(12);
  const auto scenePayload = encodeDisplayScenePayload(source);
  assert(!scenePayload.empty());
  SceneSnapshot decoded;
  assert(decodeDisplayScenePayload(scenePayload, decoded));
  assert(decoded.characterName == source.characterName);
  assert(decoded.flags == source.flags);

  TextUpdate caption{12, TextTarget::Caption, 16,
                     "接続状態を確認してみよう。"};
  const auto textPayload = encodeDisplayTextPayload(caption);
  TextUpdate decodedText;
  assert(decodeDisplayTextPayload(textPayload, decodedText));
  assert(decodedText.text == caption.text);

  SceneModel model;
  assert(model.stage(decoded) == SceneApplyResult::Ok);
  assert(model.active().revision == 0);
  assert(model.stageText(decodedText.revision, decodedText.target,
                         decodedText.fontSize, decodedText.text) ==
         SceneApplyResult::Ok);
  assert(model.commit(11) == SceneApplyResult::RevisionMismatch);
  assert(model.active().revision == 0);
  assert(model.commit(12) == SceneApplyResult::Ok);
  assert(model.active().revision == 12);
  assert(model.active().caption == caption.text);
  assert(model.dirty());

  auto stale = sampleScene(11);
  assert(model.stage(stale) == SceneApplyResult::StaleRevision);
}

MonochromeAssetMetadata metadataFor(const std::vector<uint8_t> &pixels,
                                    const char *revision,
                                    const char *frameName = "portrait") {
  MonochromeAssetMetadata metadata;
  metadata.width = 400;
  metadata.height = 300;
  metadata.byteCount = pixels.size();
  metadata.checksum =
      charadock::protocol::crc32(pixels.data(), pixels.size()) ^ 0xffffffffu;
  std::snprintf(metadata.revision.data(), metadata.revision.size(), "%s",
                revision);
  std::snprintf(metadata.frameName.data(), metadata.frameName.size(),
                "%s", frameName);
  return metadata;
}

void dispatcherRoutesAnimationFramesToIndependentStores() {
  constexpr size_t bytes = MonochromeAssetStore::kMaximumBytes;
  std::vector<uint8_t> neutralFirst(bytes), neutralSecond(bytes);
  std::vector<uint8_t> blinkFirst(bytes), blinkSecond(bytes);
  std::vector<uint8_t> halfFirst(bytes), halfSecond(bytes);
  std::vector<uint8_t> openFirst(bytes), openSecond(bytes);
  MonochromeAssetStore neutral;
  MonochromeAssetStore blink;
  MonochromeAssetStore half;
  MonochromeAssetStore open;
  assert(neutral.attach(neutralFirst.data(), neutralSecond.data(), bytes));
  assert(blink.attach(blinkFirst.data(), blinkSecond.data(), bytes));
  assert(half.attach(halfFirst.data(), halfSecond.data(), bytes));
  assert(open.attach(openFirst.data(), openSecond.data(), bytes));

  SceneModel model;
  FrameDispatcher dispatcher(model, neutral, nullptr, nullptr, &blink, &half,
                             &open);
  const std::vector<std::pair<const char *, MonochromeAssetStore *>> targets = {
      {"portrait-blink", &blink},
      {"portrait-mouth-half", &half},
      {"portrait-mouth-open", &open},
  };
  uint16_t sequence = 1;
  uint8_t fill = 0x31;
  for (const auto &[frameName, target] : targets) {
    std::vector<uint8_t> pixels(bytes, fill++);
    const auto metadata = metadataFor(pixels, frameName, frameName);
    auto outcome = dispatcher.apply(
        Frame{FrameType::AssetMeta, sequence++,
              encodeMonochromeAssetMetadata(metadata)});
    assert(outcome.result == FrameApplyResult::Applied);
    for (size_t offset = 0; offset < pixels.size();) {
      const size_t count = std::min<size_t>(4092, pixels.size() - offset);
      std::vector<uint8_t> chunk(sizeof(uint32_t) + count);
      chunk[0] = static_cast<uint8_t>(offset);
      chunk[1] = static_cast<uint8_t>(offset >> 8);
      chunk[2] = static_cast<uint8_t>(offset >> 16);
      chunk[3] = static_cast<uint8_t>(offset >> 24);
      std::copy_n(pixels.begin() + offset, count,
                  chunk.begin() + sizeof(uint32_t));
      outcome = dispatcher.apply(
          Frame{FrameType::AssetChunk, sequence++, std::move(chunk)});
      assert(outcome.result == FrameApplyResult::Applied);
      offset += count;
    }
    outcome = dispatcher.apply(Frame{FrameType::AssetEnd, sequence++, {}});
    assert(outcome.result == FrameApplyResult::AssetCompleted);
    assert(target->available());
    assert(target->pixels()[0] == pixels[0]);
  }
  assert(!neutral.available());

  auto unsupported = metadataFor(std::vector<uint8_t>(bytes), "bad",
                                 "portrait-unknown");
  const auto rejected = dispatcher.apply(
      Frame{FrameType::AssetMeta, sequence,
            encodeMonochromeAssetMetadata(unsupported)});
  assert(rejected.result == FrameApplyResult::AssetRejected);
}

void monochromeAssetCommitIsAtomic() {
  std::vector<uint8_t> first(MonochromeAssetStore::kMaximumBytes);
  std::vector<uint8_t> second(MonochromeAssetStore::kMaximumBytes);
  std::vector<uint8_t> pixels(MonochromeAssetStore::kMaximumBytes, 0x5a);
  MonochromeAssetStore store;
  assert(store.attach(first.data(), second.data(), first.size()));

  auto metadata = metadataFor(pixels, "amber:rlcd-r1");
  const auto payload = encodeMonochromeAssetMetadata(metadata);
  MonochromeAssetMetadata decoded;
  assert(decodeMonochromeAssetMetadata(payload, decoded));
  assert(store.beginTransfer(decoded) == MonochromeAssetResult::Ok);
  assert(store.writeChunk(0, pixels.data(), 4092) ==
         MonochromeAssetResult::Ok);
  assert(store.writeChunk(4100, pixels.data(), 8) ==
         MonochromeAssetResult::UnexpectedOffset);
  assert(store.writeChunk(4092, pixels.data() + 4092,
                          pixels.size() - 4092) ==
         MonochromeAssetResult::Ok);
  assert(store.finishTransfer() == MonochromeAssetResult::Ok);
  assert(store.available());
  assert(store.pixels()[0] == 0x5a);

  auto broken = metadataFor(pixels, "amber:rlcd-r2");
  broken.checksum ^= 1;
  assert(store.beginTransfer(broken) == MonochromeAssetResult::Ok);
  assert(store.writeChunk(0, pixels.data(), pixels.size()) ==
         MonochromeAssetResult::Ok);
  assert(store.finishTransfer() == MonochromeAssetResult::ChecksumMismatch);
  assert(store.available());
  assert(std::string(store.metadata()->revision.data()) == "amber:rlcd-r1");
}

void characterChangeKeepsLastVerifiedPortraitUntilCommit() {
  std::vector<uint8_t> first(MonochromeAssetStore::kMaximumBytes);
  std::vector<uint8_t> second(MonochromeAssetStore::kMaximumBytes);
  std::vector<uint8_t> oldPixels(MonochromeAssetStore::kMaximumBytes, 0x33);
  std::vector<uint8_t> newPixels(MonochromeAssetStore::kMaximumBytes, 0xcc);
  MonochromeAssetStore store;
  assert(store.attach(first.data(), second.data(), first.size()));
  auto oldMetadata = metadataFor(oldPixels, "portrait-old");
  assert(store.beginTransfer(oldMetadata) == MonochromeAssetResult::Ok);
  assert(store.writeChunk(0, oldPixels.data(), oldPixels.size()) ==
         MonochromeAssetResult::Ok);
  assert(store.finishTransfer() == MonochromeAssetResult::Ok);

  SceneModel model;
  FrameDispatcher dispatcher(model, store);
  const std::string nextRevision = "portrait-new";
  const auto changed = dispatcher.apply(Frame{
      FrameType::CharacterChanged, 1,
      std::vector<uint8_t>(nextRevision.begin(), nextRevision.end())});
  assert(changed.result == FrameApplyResult::Applied);
  assert(store.available());
  assert(store.pixels()[0] == 0x33);
  assert(store.matchesRevision(
      reinterpret_cast<const uint8_t *>("portrait-old"), 12));

  auto brokenMetadata = metadataFor(newPixels, nextRevision.c_str());
  brokenMetadata.checksum ^= 1;
  assert(store.beginTransfer(brokenMetadata) == MonochromeAssetResult::Ok);
  assert(store.writeChunk(0, newPixels.data(), newPixels.size()) ==
         MonochromeAssetResult::Ok);
  assert(store.finishTransfer() == MonochromeAssetResult::ChecksumMismatch);
  assert(store.available());
  assert(store.pixels()[0] == 0x33);
  assert(store.matchesRevision(
      reinterpret_cast<const uint8_t *>("portrait-old"), 12));
}

void hostWatchdogIsWrapSafeAndBuildsOfflineSnapshot() {
  HostConnectionWatchdog watchdog(24000);
  assert(!watchdog.hostSeen());
  assert(!watchdog.consumeTimeout(50000));

  const uint32_t nearWrap = 0xfffff000u;
  watchdog.noteActivity(nearWrap);
  assert(watchdog.online());
  assert(!watchdog.consumeTimeout(nearWrap + 23000u));
  assert(watchdog.consumeTimeout(nearWrap + 24000u));
  assert(!watchdog.online());
  assert(!watchdog.consumeTimeout(nearWrap + 25000u));

  watchdog.noteActivity(1234);
  assert(watchdog.online());

  auto current = sampleScene(44);
  current.activity = "作業中";
  current.nextAction = "次の一手";
  current.footer = "USB CONNECTED";
  const auto offline = offlineSnapshot(current);
  assert(offline.scene == SceneId::Offline);
  assert(offline.state == DeviceState::Offline);
  assert((offline.flags & SceneConnected) == 0);
  assert(offline.characterName == current.characterName);
  assert(offline.revision == current.revision);
  assert(offline.caption.empty());
  assert(offline.activity.empty());
  assert(offline.nextAction.empty());
  assert(!offline.footer.empty());
}

void dispatcherAppliesOneAtomicDisplayTransaction() {
  std::vector<uint8_t> first(MonochromeAssetStore::kMaximumBytes);
  std::vector<uint8_t> second(MonochromeAssetStore::kMaximumBytes);
  MonochromeAssetStore store;
  assert(store.attach(first.data(), second.data(), first.size()));
  SceneModel model;
  FrameDispatcher dispatcher(model, store);

  const auto scenePayload = encodeDisplayScenePayload(sampleScene(99));
  auto outcome = dispatcher.apply(
      Frame{FrameType::DisplayScene, 1, scenePayload});
  assert(outcome.result == FrameApplyResult::Applied);
  assert(!outcome.displayChanged);

  const TextUpdate activity{99, TextTarget::Caption, 16,
                            "音声経路を確認しています。"};
  outcome = dispatcher.apply(
      Frame{FrameType::DisplayText, 2, encodeDisplayTextPayload(activity)});
  assert(outcome.result == FrameApplyResult::Applied);
  assert(model.active().revision == 0);

  outcome = dispatcher.apply(
      Frame{FrameType::DisplayCommit, 3, encodeDisplayCommitPayload(99)});
  assert(outcome.result == FrameApplyResult::Applied);
  assert(outcome.displayChanged);
  assert(model.active().revision == 99);
  assert(model.active().caption == activity.text);

  outcome = dispatcher.apply(Frame{FrameType::State, 4,
                                   {static_cast<uint8_t>(DeviceState::Idle)}});
  assert(outcome.result == FrameApplyResult::Applied);
  assert(model.active().state == DeviceState::Idle);
  outcome = dispatcher.apply(Frame{FrameType::State, 5, {255}});
  assert(outcome.result == FrameApplyResult::InvalidPayload);
}

void dispatcherValidatesAndRoutesPcmAudio() {
  std::vector<uint8_t> first(MonochromeAssetStore::kMaximumBytes);
  std::vector<uint8_t> second(MonochromeAssetStore::kMaximumBytes);
  MonochromeAssetStore store;
  assert(store.attach(first.data(), second.data(), first.size()));
  SceneModel model;
  FakeAudioSink audio;
  FrameDispatcher dispatcher(model, store, nullptr, &audio);

  std::vector<uint8_t> begin(8);
  begin[0] = 0x80;
  begin[1] = 0x3e; // 16000 little endian
  begin[4] = 0x00;
  begin[5] = 0x04; // 1024 samples
  auto outcome = dispatcher.apply(Frame{FrameType::AudioBegin, 1, begin});
  assert(outcome.result == FrameApplyResult::Applied);
  assert(outcome.audioResult == 0);
  assert(outcome.displayChanged);
  assert(model.active().state == DeviceState::Speaking);
  assert(audio.rate == 16000 && audio.expected == 1024);

  outcome = dispatcher.apply(
      Frame{FrameType::AudioChunk, 2, std::vector<uint8_t>(2048, 0x5a)});
  assert(outcome.result == FrameApplyResult::Applied);
  assert(audio.bytes == 2048);
  outcome = dispatcher.apply(Frame{FrameType::AudioEnd, 3, {}});
  assert(outcome.result == FrameApplyResult::Applied);
  assert(audio.finishes == 1);
  outcome = dispatcher.apply(Frame{FrameType::AudioStop, 4, {}});
  assert(outcome.result == FrameApplyResult::Applied);
  assert(audio.stops == 1);
  assert(model.active().state == DeviceState::Idle);

  outcome = dispatcher.apply(Frame{FrameType::AudioChunk, 5, {1}});
  assert(outcome.result == FrameApplyResult::AudioRejected);
  assert(outcome.audioResult ==
         static_cast<uint8_t>(AudioApplyResult::InvalidFormat));

  audio.startResult = AudioApplyResult::CodecFailure;
  outcome = dispatcher.apply(Frame{FrameType::AudioBegin, 6, begin});
  assert(outcome.result == FrameApplyResult::AudioRejected);
  assert(outcome.audioResult ==
         static_cast<uint8_t>(AudioApplyResult::CodecFailure));
}

int main() {
  protocolRoundTripIsFragmentSafe();
  utf8ValidationRejectsControlsAndMalformedText();
  generatedShinonomeFontsAreBoundedAndSearchable();
  buttonFeedbackDebouncesAndLongPressIsInterruptible();
  scenePayloadsCommitAtomically();
  monochromeAssetCommitIsAtomic();
  dispatcherRoutesAnimationFramesToIndependentStores();
  characterChangeKeepsLastVerifiedPortraitUntilCommit();
  hostWatchdogIsWrapSafeAndBuildsOfflineSnapshot();
  dispatcherAppliesOneAtomicDisplayTransaction();
  dispatcherValidatesAndRoutesPcmAudio();
  std::cout << "RLCD protocol/model tests passed\n";
  return 0;
}
