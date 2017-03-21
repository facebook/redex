/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#include "Match.h"

namespace m {

match_t<DexInstruction, std::tuple<match_t<DexInstruction> > >
  invoke_static() {
    return invoke_static(any<DexInstruction>());
};

match_t<DexInstruction> return_void() {
  return {
    [](const DexInstruction* insn) {
      auto opcode = insn->opcode();
      return opcode == OPCODE_RETURN_VOID;
    }
  };
}

match_t<DexInstruction, std::tuple<int> > has_n_args(int n) {
  return {
    // N.B. "int n" must be const ref in order to appease N-ary matcher template
    [](const DexInstruction* insn, const int& n) {
      assert(insn->has_arg_word_count() || insn->has_range());
      if (insn->has_arg_word_count()) {
        return insn->arg_word_count() == n;
      } else if (insn->has_range()) {
        // N.B. seems like invoke-*/range should never occur with 0 args,
        // so let's make sure this assumption holds...
        assert(insn->range_size() > 0);
        return insn->range_size() == n;
      } else {
        assert(false);
      }
    },
    n
  };
}

match_t<DexClass, std::tuple<> > is_interface() {
  return {
    [](const DexClass* cls) {
      return (bool)(cls->get_access() & ACC_INTERFACE);
    }
  };
}

match_t<DexInstruction> has_types() {
  return {
    [](const DexInstruction* insn) {
      return insn->has_types();
    }
  };
}

match_t<DexInstruction> const_string() {
  return {
    [](const DexInstruction* insn) {
      auto opcode = insn->opcode();
      return opcode == OPCODE_CONST_STRING ||
        opcode == OPCODE_CONST_STRING_JUMBO;
    }
  };
}

match_t<DexInstruction, std::tuple<match_t<DexInstruction> > >
  new_instance() {
    return new_instance(any<DexInstruction>());
}

match_t<DexInstruction> throwex() {
  return {
    [](const DexInstruction* insn) {
      auto opcode = insn->opcode();
      return opcode == OPCODE_THROW;
    }
  };
}

 match_t<DexInstruction, std::tuple<match_t<DexInstruction> > >
  invoke_direct() {
    return invoke_direct(any<DexInstruction>());
};

match_t<DexMethod, std::tuple<> > is_default_constructor() {
  return {
    [](const DexMethod* meth) {
      if (!is_static(meth) &&
              is_constructor(meth) &&
              has_no_args(meth) &&
              has_code(meth)) {
        auto ii = InstructionIterable(meth->get_code()->get_entries());
        auto it = ii.begin();
        auto end = ii.end();
        auto op = it->insn->opcode();
        if (op != OPCODE_INVOKE_DIRECT && op != OPCODE_INVOKE_STATIC_RANGE) {
          return false;
        }
        ++it;
        if (it->insn->opcode() != OPCODE_RETURN_VOID) {
          return false;
        }
        ++it;
        if (it != end) {
          return false;
        }
      }
      return false;
    }
  };
}

match_t<DexMethod, std::tuple<> > is_constructor() {
  return {
    [](const DexMethod* meth) {
      return is_constructor(meth);
    }
  };
}

match_t<DexClass, std::tuple<> > is_enum() {
  return {
    [](const DexClass* cls) {
      return (bool)(cls->get_access() & ACC_ENUM);
    }
  };
}

match_t<DexClass, std::tuple<> > has_class_data() {
  return {
    [](const DexClass* cls) {
      return cls->has_class_data();
    }
  };
}

bool is_assignable_to_interface(const DexType* type, const DexType* iface) {
  if (type == iface) return true;
  auto cls = type_class(type);
  if (cls) {
    for (auto extends : cls->get_interfaces()->get_type_list()) {
      if (is_assignable_to_interface(extends, iface)) {
        return true;
      }
    }
  }
  return false;
}

bool is_assignable_to(const DexType* child, const DexType* parent) {
  // Check class hierarchy
  auto super = child;
  while (super != nullptr) {
    if (parent == super) return true;
    const auto cls = type_class(super);
    if (cls == nullptr) break;
    super = cls->get_super_class();
  }
  // Check interface hierarchy
  DexClass* parent_cls = type_class(parent);
  return parent_cls &&
    is_interface(parent_cls) &&
    is_assignable_to_interface(child, parent);
}

match_t<DexType, std::tuple<const DexType*> >
  is_assignable_to(const DexType* parent) {
  return {
    [](const DexType* child, const DexType* const& parent) {
      return is_assignable_to(child, parent);
    },
    parent
  };
}

}
