/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ConstantPropagationAnalysis.h"

#include <unordered_set>

#include "SignedConstantDomain.h"

namespace constant_propagation {

// TODO(T257927964): Remove this.
bool known_non_null_returns_enable = false;

namespace {

const std::unordered_set<const DexMethodRef*>& known_non_null_return_methods() {
  // Lazily initialized on first call. DexMethod::make_method acquires a global
  // lock, so we avoid doing this at static init time.
  //
  // Safety criterion: only include methods that are static, final, or defined
  // on a final class. Virtual methods on non-final classes are unsafe because
  // a subclass could override them to return null, and we match on the
  // DexMethodRef from the call site, which resolves to the override at runtime.
  // Checking the full class hierarchy is not worthwhile here because these are
  // external methods whose subclasses are not all visible to Redex. For
  // app-internal methods, interprocedural constant propagation already analyzes
  // method bodies and can determine non-null returns without a hardcoded list.
  static const auto* methods = []() {
    auto* s = new std::unordered_set<const DexMethodRef*>();
    for (const char* sig : {
             // Java boxing methods: always return a new or cached wrapper
             // object (JLS 5.1.7).
             "Ljava/lang/Boolean;.valueOf:(Z)Ljava/lang/Boolean;",
             "Ljava/lang/Byte;.valueOf:(B)Ljava/lang/Byte;",
             "Ljava/lang/Character;.valueOf:(C)Ljava/lang/Character;",
             "Ljava/lang/Double;.valueOf:(D)Ljava/lang/Double;",
             "Ljava/lang/Float;.valueOf:(F)Ljava/lang/Float;",
             "Ljava/lang/Integer;.valueOf:(I)Ljava/lang/Integer;",
             "Ljava/lang/Long;.valueOf:(J)Ljava/lang/Long;",
             "Ljava/lang/Short;.valueOf:(S)Ljava/lang/Short;",

             // Object.getClass: final native, always returns non-null.
             "Ljava/lang/Object;.getClass:()Ljava/lang/Class;",

             // Class accessors: final class, always return non-null per
             // spec. Note: getCanonicalName() is NOT safe -- it returns
             // null for local/anonymous classes and for arrays whose
             // component type lacks a canonical name.
             "Ljava/lang/Class;.getName:()Ljava/lang/String;",
             "Ljava/lang/Class;.getSimpleName:()Ljava/lang/String;",

             // Thread.currentThread: static native, always returns non-null.
             "Ljava/lang/Thread;.currentThread:()Ljava/lang/Thread;",

             // Enum.name: final, set by the compiler, always non-null.
             "Ljava/lang/Enum;.name:()Ljava/lang/String;",

             // Optional / OptionalInt / OptionalLong / OptionalDouble:
             // all final classes. Factories are non-null by construction;
             // `of` throws NPE on null rather than returning null.
             // `Optional.get()` throws NoSuchElementException on empty,
             // and the only ways to populate an Optional disallow null
             // contents, so the returned value is non-null.
             ("Ljava/util/Optional;.of:"
              "(Ljava/lang/Object;)Ljava/util/Optional;"),
             ("Ljava/util/Optional;.ofNullable:"
              "(Ljava/lang/Object;)Ljava/util/Optional;"),
             "Ljava/util/Optional;.empty:()Ljava/util/Optional;",
             "Ljava/util/Optional;.get:()Ljava/lang/Object;",
             "Ljava/util/OptionalInt;.of:(I)Ljava/util/OptionalInt;",
             "Ljava/util/OptionalInt;.empty:()Ljava/util/OptionalInt;",
             "Ljava/util/OptionalLong;.of:(J)Ljava/util/OptionalLong;",
             "Ljava/util/OptionalLong;.empty:()Ljava/util/OptionalLong;",
             ("Ljava/util/OptionalDouble;.of:"
              "(D)Ljava/util/OptionalDouble;"),
             ("Ljava/util/OptionalDouble;.empty:"
              "()Ljava/util/OptionalDouble;"),

             // Objects: final class, all entries here are static.
             // requireNonNull throws NPE on null. toString(Object) returns
             // the "null" literal for a null arg; the 2-arg overload is
             // NOT safe because the default can itself be null.
             ("Ljava/util/Objects;.toString:"
              "(Ljava/lang/Object;)Ljava/lang/String;"),
             ("Ljava/util/Objects;.requireNonNull:"
              "(Ljava/lang/Object;)Ljava/lang/Object;"),
             ("Ljava/util/Objects;.requireNonNull:"
              "(Ljava/lang/Object;Ljava/lang/String;)Ljava/lang/Object;"),
             ("Ljava/util/Objects;.requireNonNull:(Ljava/lang/Object;"
              "Ljava/util/function/Supplier;)Ljava/lang/Object;"),

             // String instance methods.
             ("Ljava/lang/String;.toLowerCase:"
              "(Ljava/util/Locale;)Ljava/lang/String;"),
             "Ljava/lang/String;.toLowerCase:()Ljava/lang/String;",
             ("Ljava/lang/String;.toUpperCase:"
              "(Ljava/util/Locale;)Ljava/lang/String;"),
             "Ljava/lang/String;.toUpperCase:()Ljava/lang/String;",
             "Ljava/lang/String;.substring:(I)Ljava/lang/String;",
             "Ljava/lang/String;.substring:(II)Ljava/lang/String;",
             "Ljava/lang/String;.trim:()Ljava/lang/String;",
             "Ljava/lang/String;.strip:()Ljava/lang/String;",
             "Ljava/lang/String;.stripLeading:()Ljava/lang/String;",
             "Ljava/lang/String;.stripTrailing:()Ljava/lang/String;",
             "Ljava/lang/String;.repeat:(I)Ljava/lang/String;",
             "Ljava/lang/String;.intern:()Ljava/lang/String;",
             "Ljava/lang/String;.toCharArray:()[C",
             "Ljava/lang/String;.replace:(CC)Ljava/lang/String;",
             ("Ljava/lang/String;.replace:(Ljava/lang/CharSequence;"
              "Ljava/lang/CharSequence;)Ljava/lang/String;"),
             ("Ljava/lang/String;.replaceAll:(Ljava/lang/String;"
              "Ljava/lang/String;)Ljava/lang/String;"),
             ("Ljava/lang/String;.replaceFirst:(Ljava/lang/String;"
              "Ljava/lang/String;)Ljava/lang/String;"),
             ("Ljava/lang/String;.split:"
              "(Ljava/lang/String;)[Ljava/lang/String;"),
             ("Ljava/lang/String;.split:"
              "(Ljava/lang/String;I)[Ljava/lang/String;"),
             ("Ljava/lang/String;.concat:"
              "(Ljava/lang/String;)Ljava/lang/String;"),
             ("Ljava/lang/String;.getBytes:"
              "(Ljava/nio/charset/Charset;)[B"),
             "Ljava/lang/String;.getBytes:(Ljava/lang/String;)[B",
             "Ljava/lang/String;.getBytes:()[B",

             // String static methods.
             ("Ljava/lang/String;.format:(Ljava/lang/String;"
              "[Ljava/lang/Object;)Ljava/lang/String;"),
             ("Ljava/lang/String;.format:(Ljava/util/Locale;"
              "Ljava/lang/String;[Ljava/lang/Object;)Ljava/lang/String;"),
             ("Ljava/lang/String;.valueOf:"
              "(Ljava/lang/Object;)Ljava/lang/String;"),
             "Ljava/lang/String;.valueOf:(Z)Ljava/lang/String;",
             "Ljava/lang/String;.valueOf:(C)Ljava/lang/String;",
             "Ljava/lang/String;.valueOf:(D)Ljava/lang/String;",
             "Ljava/lang/String;.valueOf:(F)Ljava/lang/String;",
             "Ljava/lang/String;.valueOf:(I)Ljava/lang/String;",
             "Ljava/lang/String;.valueOf:(J)Ljava/lang/String;",
             "Ljava/lang/String;.valueOf:([C)Ljava/lang/String;",
             ("Ljava/lang/String;.valueOf:"
              "([CII)Ljava/lang/String;"),
             "Ljava/lang/String;.copyValueOf:([C)Ljava/lang/String;",
             ("Ljava/lang/String;.copyValueOf:"
              "([CII)Ljava/lang/String;"),
             ("Ljava/lang/String;.join:(Ljava/lang/CharSequence;"
              "[Ljava/lang/CharSequence;)Ljava/lang/String;"),
             ("Ljava/lang/String;.join:(Ljava/lang/CharSequence;"
              "Ljava/lang/Iterable;)Ljava/lang/String;"),

             // Java Collections factories: always return a new collection.
             ("Ljava/util/Collections;.singletonList:"
              "(Ljava/lang/Object;)Ljava/util/List;"),
             ("Ljava/util/Collections;.singleton:"
              "(Ljava/lang/Object;)Ljava/util/Set;"),
             ("Ljava/util/Collections;.singletonMap:(Ljava/lang/Object;"
              "Ljava/lang/Object;)Ljava/util/Map;"),
             ("Ljava/util/Collections;.unmodifiableList:"
              "(Ljava/util/List;)Ljava/util/List;"),
             ("Ljava/util/Collections;.unmodifiableSet:"
              "(Ljava/util/Set;)Ljava/util/Set;"),
             ("Ljava/util/Collections;.unmodifiableMap:"
              "(Ljava/util/Map;)Ljava/util/Map;"),
             "Ljava/util/Collections;.emptyList:()Ljava/util/List;",
             "Ljava/util/Collections;.emptySet:()Ljava/util/Set;",
             "Ljava/util/Collections;.emptyMap:()Ljava/util/Map;",

             // Arrays.asList: static on a final class, returns a fresh
             // private ArrayList wrapper around the input array.
             ("Ljava/util/Arrays;.asList:"
              "([Ljava/lang/Object;)Ljava/util/List;"),

             // EnumSet.of: always returns a new EnumSet.
             ("Ljava/util/EnumSet;.of:"
              "(Ljava/lang/Enum;)Ljava/util/EnumSet;"),
             ("Ljava/util/EnumSet;.of:(Ljava/lang/Enum;"
              "Ljava/lang/Enum;)Ljava/util/EnumSet;"),
             ("Ljava/util/EnumSet;.of:(Ljava/lang/Enum;Ljava/lang/Enum;"
              "Ljava/lang/Enum;)Ljava/util/EnumSet;"),
             ("Ljava/util/EnumSet;.of:(Ljava/lang/Enum;Ljava/lang/Enum;"
              "Ljava/lang/Enum;Ljava/lang/Enum;)Ljava/util/EnumSet;"),
             ("Ljava/util/EnumSet;.of:(Ljava/lang/Enum;Ljava/lang/Enum;"
              "Ljava/lang/Enum;Ljava/lang/Enum;Ljava/lang/Enum;)"
              "Ljava/util/EnumSet;"),
             ("Ljava/util/EnumSet;.of:(Ljava/lang/Enum;"
              "[Ljava/lang/Enum;)Ljava/util/EnumSet;"),

             // Android SDK: documented @NonNull ("This value cannot be null").
             "Landroid/view/View;.requireViewById:(I)Landroid/view/View;",
             "Landroid/app/Activity;.requireViewById:(I)Landroid/view/View;",
             "Landroid/app/Dialog;.requireViewById:(I)Landroid/view/View;",
             "Landroid/content/Context;.getString:(I)Ljava/lang/String;",
             ("Landroid/content/Context;.getString:"
              "(I[Ljava/lang/Object;)Ljava/lang/String;"),

             // Guava ImmutableList: annotated @NonNull.
             ("Lcom/google/common/collect/ImmutableList;.of:()"
              "Lcom/google/common/collect/ImmutableList;"),
             ("Lcom/google/common/collect/ImmutableList;.of:"
              "(Ljava/lang/Object;)"
              "Lcom/google/common/collect/ImmutableList;"),
             ("Lcom/google/common/collect/ImmutableList;.of:"
              "(Ljava/lang/Object;Ljava/lang/Object;)"
              "Lcom/google/common/collect/ImmutableList;"),
             ("Lcom/google/common/collect/ImmutableList;.of:"
              "(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;)"
              "Lcom/google/common/collect/ImmutableList;"),
             ("Lcom/google/common/collect/ImmutableList;.of:"
              "(Ljava/lang/Object;Ljava/lang/Object;"
              "Ljava/lang/Object;Ljava/lang/Object;)"
              "Lcom/google/common/collect/ImmutableList;"),
             ("Lcom/google/common/collect/ImmutableList;.of:"
              "(Ljava/lang/Object;Ljava/lang/Object;"
              "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;)"
              "Lcom/google/common/collect/ImmutableList;"),
             ("Lcom/google/common/collect/ImmutableList;.of:"
              "(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
              "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;)"
              "Lcom/google/common/collect/ImmutableList;"),
             ("Lcom/google/common/collect/ImmutableList;.of:"
              "(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
              "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
              "Ljava/lang/Object;)"
              "Lcom/google/common/collect/ImmutableList;"),
             ("Lcom/google/common/collect/ImmutableList;.of:"
              "(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
              "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
              "Ljava/lang/Object;Ljava/lang/Object;)"
              "Lcom/google/common/collect/ImmutableList;"),
             ("Lcom/google/common/collect/ImmutableList;.of:"
              "(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
              "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
              "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;)"
              "Lcom/google/common/collect/ImmutableList;"),
             ("Lcom/google/common/collect/ImmutableList;.of:"
              "(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
              "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
              "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
              "Ljava/lang/Object;Ljava/lang/Object;)"
              "Lcom/google/common/collect/ImmutableList;"),
             ("Lcom/google/common/collect/ImmutableList;.of:"
              "(Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
              "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
              "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
              "Ljava/lang/Object;Ljava/lang/Object;Ljava/lang/Object;"
              "[Ljava/lang/Object;)"
              "Lcom/google/common/collect/ImmutableList;"),
             ("Lcom/google/common/collect/ImmutableList;.copyOf:"
              "(Ljava/util/Collection;)"
              "Lcom/google/common/collect/ImmutableList;"),
             ("Lcom/google/common/collect/ImmutableList;.copyOf:"
              "(Ljava/lang/Iterable;)"
              "Lcom/google/common/collect/ImmutableList;"),
             ("Lcom/google/common/collect/ImmutableList;.copyOf:"
              "(Ljava/util/Iterator;)"
              "Lcom/google/common/collect/ImmutableList;"),
             ("Lcom/google/common/collect/ImmutableList;.copyOf:"
              "([Ljava/lang/Object;)"
              "Lcom/google/common/collect/ImmutableList;"),

             // toString() on selected final JDK classes: always returns
             // non-null. Only final classes are safe here because virtual
             // dispatch selects the runtime type's implementation, and a
             // subclass could override toString() to return null.
             "Ljava/lang/StringBuilder;.toString:()Ljava/lang/String;",
             "Ljava/lang/StringBuffer;.toString:()Ljava/lang/String;",
             "Ljava/lang/String;.toString:()Ljava/lang/String;",
             "Ljava/lang/Boolean;.toString:()Ljava/lang/String;",
             "Ljava/lang/Byte;.toString:()Ljava/lang/String;",
             "Ljava/lang/Character;.toString:()Ljava/lang/String;",
             "Ljava/lang/Class;.toString:()Ljava/lang/String;",
             "Ljava/lang/Double;.toString:()Ljava/lang/String;",
             "Ljava/lang/Float;.toString:()Ljava/lang/String;",
             "Ljava/lang/Integer;.toString:()Ljava/lang/String;",
             "Ljava/lang/Long;.toString:()Ljava/lang/String;",
             "Ljava/lang/Short;.toString:()Ljava/lang/String;",
             "Ljava/util/UUID;.toString:()Ljava/lang/String;",

             // Guava ImmutableMap: annotated @NonNull.
             ("Lcom/google/common/collect/ImmutableMap;.of:()"
              "Lcom/google/common/collect/ImmutableMap;"),
             ("Lcom/google/common/collect/ImmutableMap;.of:"
              "(Ljava/lang/Object;Ljava/lang/Object;)"
              "Lcom/google/common/collect/ImmutableMap;"),
             ("Lcom/google/common/collect/ImmutableMap;.of:"
              "(Ljava/lang/Object;Ljava/lang/Object;"
              "Ljava/lang/Object;Ljava/lang/Object;)"
              "Lcom/google/common/collect/ImmutableMap;"),
             ("Lcom/google/common/collect/ImmutableMap;.of:"
              "(Ljava/lang/Object;Ljava/lang/Object;"
              "Ljava/lang/Object;Ljava/lang/Object;"
              "Ljava/lang/Object;Ljava/lang/Object;)"
              "Lcom/google/common/collect/ImmutableMap;"),
             ("Lcom/google/common/collect/ImmutableMap;.copyOf:"
              "(Ljava/util/Map;)"
              "Lcom/google/common/collect/ImmutableMap;"),
         }) {
      s->insert(DexMethod::make_method(sig));
    }
    return s;
  }();
  return *methods;
}

} // namespace

bool KnownNonNullReturnsAnalyzer::analyze_invoke(const IRInstruction* insn,
                                                 ConstantEnvironment* env) {
  if (!known_non_null_returns_enable) {
    return false;
  }
  auto* method = insn->get_method();
  if (method != nullptr && known_non_null_return_methods().contains(method)) {
    // NEZ (not-equal-to-zero) is the right abstraction here: we only know the
    // return value is non-null, not its concrete type or allocation site.
    // Object domains (NewObjectDomain, etc.) would carry richer information
    // that we don't have for external methods.
    env->set(RESULT_REGISTER, SignedConstantDomain(sign_domain::Interval::NEZ));
    return true;
  }
  return false;
}

} // namespace constant_propagation
