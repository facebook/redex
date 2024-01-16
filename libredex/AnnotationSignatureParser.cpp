/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "AnnotationSignatureParser.h"
#include "Show.h"
#include "Trace.h"

namespace annotation_signature_parser {

void parse(
    const DexAnnotation* anno,
    const std::function<bool(const DexEncodedValueString*, DexClass*)>& pred) {
  TRACE(ANNO, 3, "Examining @Signature instance %s", SHOW(anno));
  const auto& elems = anno->anno_elems();
  for (const auto& elem : elems) {
    const auto& ev = elem.encoded_value;
    if (ev->evtype() != DEVT_ARRAY) continue;
    auto arrayev = static_cast<DexEncodedValueArray*>(ev.get());
    auto const& evs = arrayev->evalues();
    for (auto& strev : *evs) {
      if (strev->evtype() != DEVT_STRING) continue;
      auto* devs = static_cast<DexEncodedValueString*>(strev.get());
      const std::string sigstr = devs->string()->str_copy();
      always_assert(sigstr.length() > 0);
      const auto* sigcstr = sigstr.c_str();
      // @Signature grammar is non-trivial[1], nevermind the fact that
      // Signatures are broken up into arbitrary arrays of strings concatenated
      // at runtime. It seems like types are reliably never broken apart, so we
      // can usually find an entire type name in each DexEncodedValueString.
      //
      // We also crudely approximate that something looks like a typename in the
      // first place since there's a lot of mark up in the @Signature grammar,
      // e.g. formal type parameter names. We look for things that look like
      // "L*/*", don't include ":" (formal type parameter separator), and may or
      // may not end with a semicolon or angle bracket.
      //
      // I'm working on a C++ port of the AOSP generic signature parser so we
      // can make this more robust in the future.
      //
      // [1] androidxref.com/8.0.0_r4/xref/libcore/luni/src/main/java/libcore/
      //     reflect/GenericSignatureParser.java
      if (sigstr[0] == 'L' && strchr(sigcstr, '/') && !strchr(sigcstr, ':')) {
        auto* sigtype = DexType::get_type(sigstr);
        if (!sigtype) {
          // Try with semicolon.
          sigtype = DexType::get_type(sigstr + ';');
        }
        if (!sigtype && sigstr.back() == '<') {
          // Try replacing angle bracket with semicolon
          // d8 often encodes signature annotations this way
          std::string copy = str_copy(sigstr);
          copy.pop_back();
          copy.push_back(';');
          sigtype = DexType::get_type(copy);
        }
        if (!pred(devs, type_class(sigtype))) {
          return;
        }
      }
    }
  }
}

} // namespace annotation_signature_parser
