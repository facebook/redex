// Copyright 2004-present Facebook. All Rights Reserved.

#ifndef FLATBUFFERS_GENERATED_NATIVEOUTLINER_FACEBOOK_REDEX_OUTLINER_H_
#define FLATBUFFERS_GENERATED_NATIVEOUTLINER_FACEBOOK_REDEX_OUTLINER_H_

#include "flatbuffers/flatbuffers.h"


namespace facebook {
namespace redex {
namespace outliner {

struct OutlinedThrow;
struct OutlinedThrows;

struct OutlinedThrow FLATBUFFERS_FINAL_CLASS : private flatbuffers::Table {
  int32_t location() const { return GetField<int32_t>(4, 0); }
  const flatbuffers::String *type() const { return GetPointer<const flatbuffers::String *>(6); }
  const flatbuffers::String *msg() const { return GetPointer<const flatbuffers::String *>(8); }
  bool Verify(flatbuffers::Verifier &verifier) const {
    return VerifyTableStart(verifier) &&
           VerifyField<int32_t>(verifier, 4 /* location */) &&
           VerifyField<flatbuffers::uoffset_t>(verifier, 6 /* type */) &&
           verifier.Verify(type()) &&
           VerifyField<flatbuffers::uoffset_t>(verifier, 8 /* msg */) &&
           verifier.Verify(msg()) &&
           verifier.EndTable();
  }
};

struct OutlinedThrowBuilder {
  flatbuffers::FlatBufferBuilder &fbb_;
  flatbuffers::uoffset_t start_;
  void add_location(int32_t location) { fbb_.AddElement<int32_t>(4, location, 0); }
  void add_type(flatbuffers::Offset<flatbuffers::String> type) { fbb_.AddOffset(6, type); }
  void add_msg(flatbuffers::Offset<flatbuffers::String> msg) { fbb_.AddOffset(8, msg); }
  OutlinedThrowBuilder(flatbuffers::FlatBufferBuilder &_fbb) : fbb_(_fbb) { start_ = fbb_.StartTable(); }
  OutlinedThrowBuilder &operator=(const OutlinedThrowBuilder &);
  flatbuffers::Offset<OutlinedThrow> Finish() {
    auto o = flatbuffers::Offset<OutlinedThrow>(fbb_.EndTable(start_, 3));
    return o;
  }
};

inline flatbuffers::Offset<OutlinedThrow> CreateOutlinedThrow(flatbuffers::FlatBufferBuilder &_fbb,
   int32_t location = 0,
   flatbuffers::Offset<flatbuffers::String> type = 0,
   flatbuffers::Offset<flatbuffers::String> msg = 0) {
  OutlinedThrowBuilder builder_(_fbb);
  builder_.add_msg(msg);
  builder_.add_type(type);
  builder_.add_location(location);
  return builder_.Finish();
}

struct OutlinedThrows FLATBUFFERS_FINAL_CLASS : private flatbuffers::Table {
  const flatbuffers::Vector<flatbuffers::Offset<OutlinedThrow>> *outlined_throws() const { return GetPointer<const flatbuffers::Vector<flatbuffers::Offset<OutlinedThrow>> *>(4); }
  bool Verify(flatbuffers::Verifier &verifier) const {
    return VerifyTableStart(verifier) &&
           VerifyField<flatbuffers::uoffset_t>(verifier, 4 /* outlined_throws */) &&
           verifier.Verify(outlined_throws()) &&
           verifier.VerifyVectorOfTables(outlined_throws()) &&
           verifier.EndTable();
  }
};

struct OutlinedThrowsBuilder {
  flatbuffers::FlatBufferBuilder &fbb_;
  flatbuffers::uoffset_t start_;
  void add_outlined_throws(flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<OutlinedThrow>>> outlined_throws) { fbb_.AddOffset(4, outlined_throws); }
  OutlinedThrowsBuilder(flatbuffers::FlatBufferBuilder &_fbb) : fbb_(_fbb) { start_ = fbb_.StartTable(); }
  OutlinedThrowsBuilder &operator=(const OutlinedThrowsBuilder &);
  flatbuffers::Offset<OutlinedThrows> Finish() {
    auto o = flatbuffers::Offset<OutlinedThrows>(fbb_.EndTable(start_, 1));
    return o;
  }
};

inline flatbuffers::Offset<OutlinedThrows> CreateOutlinedThrows(flatbuffers::FlatBufferBuilder &_fbb,
   flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<OutlinedThrow>>> outlined_throws = 0) {
  OutlinedThrowsBuilder builder_(_fbb);
  builder_.add_outlined_throws(outlined_throws);
  return builder_.Finish();
}

inline const facebook::redex::outliner::OutlinedThrows *GetOutlinedThrows(const void *buf) { return flatbuffers::GetRoot<facebook::redex::outliner::OutlinedThrows>(buf); }

inline bool VerifyOutlinedThrowsBuffer(flatbuffers::Verifier &verifier) { return verifier.VerifyBuffer<facebook::redex::outliner::OutlinedThrows>(); }

inline void FinishOutlinedThrowsBuffer(flatbuffers::FlatBufferBuilder &fbb, flatbuffers::Offset<facebook::redex::outliner::OutlinedThrows> root) { fbb.Finish(root); }

}  // namespace outliner
}  // namespace redex
}  // namespace facebook

#endif  // FLATBUFFERS_GENERATED_NATIVEOUTLINER_FACEBOOK_REDEX_OUTLINER_H_
