/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cmath>
#include <numeric>

#include "ApiLevelChecker.h"
#include "Creators.h"
#include "CrossDexRelocator.h"
#include "DexUtil.h"
#include "Mutators.h"
#include "ReachableClasses.h"
#include "VirtualScope.h"
#include "Walkers.h"

namespace interdex {
void CrossDexRelocator::gather_possibly_relocatable_methods(
    DexClass* cls, std::vector<DexMethod*>& possibly_relocatable_methods) {
  if (cls->is_external()) {
    return;
  }

  // We do not relocate static methods in the presence of a clinit, just in case
  // that clinit has some external side effects.
  bool relocate_static_methods =
      m_config.relocate_static_methods && !cls->get_clinit();
  bool relocate_non_static_direct_methods =
      m_config.relocate_non_static_direct_methods;
  bool relocate_virtual_methods = m_config.relocate_virtual_methods;

  auto can_relocate_common = [](DexMethod* m) {
    return m->is_concrete() && m->get_code() && can_rename(m) && !root(m) &&
           !m->rstate.no_optimizations() && no_invoke_super(m);
  };

  if (relocate_static_methods || relocate_non_static_direct_methods) {
    for (DexMethod* m : cls->get_dmethods()) {
      if (((relocate_static_methods && is_static(m)) ||
           (relocate_non_static_direct_methods && !is_static(m) &&
            !is_init(m))) &&
          can_relocate_common(m)) {
        possibly_relocatable_methods.push_back(m);
      }
    }
  }

  if (relocate_virtual_methods) {
    for (DexMethod* m : cls->get_vmethods()) {
      if (can_relocate_common(m)) {
        // Limitation: We only support non-true virtuals.
        auto virt_scope = m_type_system.find_virtual_scope(m);
        if (virt_scope != nullptr && is_non_virtual_scope(virt_scope)) {
          possibly_relocatable_methods.push_back(m);
        }
      }
    }
  }
}

bool CrossDexRelocator::handle_invoked_direct_methods_that_prevent_relocation(
    DexMethod* m,
    std::unordered_map<DexMethod*, DexClass*>& relocated_methods) {
  std::unordered_set<DexMethodRef*> direct_methods_preventing_relocation;
  if (gather_invoked_direct_methods_that_prevent_relocation(
          m, &direct_methods_preventing_relocation)) {
    always_assert(direct_methods_preventing_relocation.size() == 0);
    // No issues with direct methods.
    return true;
  }

  always_assert(direct_methods_preventing_relocation.size() > 0);
  if (std::any_of(direct_methods_preventing_relocation.begin(),
                  direct_methods_preventing_relocation.end(),
                  [&relocated_methods](DexMethodRef* mref) {
                    return !mref->is_def() ||
                           !relocated_methods.count(
                               static_cast<DexMethod*>(mref));
                  })) {
    // If a direct method that gets invoked isn't getting relocated itself, then
    // then we give up
    return false;
  }

  // So some direct methods which get relocated themselves are getting invoked.
  // Let's mark those direct methods, as we are creating a dependency on them
  // getting invoked, and thus we can't re-relocate them back later.
  // TODO: Track dependencies at more fine-grained level, and use that
  // information to turn more eventually unrelocated static methods back into
  // non-static direct methods.
  for (DexMethodRef* mref : direct_methods_preventing_relocation) {
    always_assert(mref->is_def());
    DexClass* relocated_cls =
        relocated_methods.at(static_cast<DexMethod*>(mref));
    RelocatedMethodInfo& info = m_relocated_method_infos.at(relocated_cls);
    info.is_dependent_non_static_direct = true;
  }
  return true;
}

std::string CrossDexRelocator::create_new_type_name(RelocatedMethodKind kind) {
  std::stringstream ss;
  ss << "Lredex/$Relocated"
     << (kind == RelocatedMethodKind::Static
             ? "Static"
             : kind == RelocatedMethodKind::NonStaticDirect ? "NonStaticDirect"
                                                            : "Virtual")
     << std::to_string(m_next_method_id++) << ";";
  return ss.str();
}

void CrossDexRelocator::relocate_methods(
    DexClass* cls, std::vector<DexClass*>& relocated_classes) {
  // Let's identify some methods that we can freely relocate!
  // For each relocatable method, we are going to create a separate
  // class, just to hold that relocatable method. This enables us to use the
  // existing class-based infrastructure to prioritize these methods.
  // Don't worry, later we are going to erase most of those classes again,
  // consolidating the relocated methods in just a few classes.
  std::vector<DexMethod*> possibly_relocatable_methods;
  gather_possibly_relocatable_methods(cls, possibly_relocatable_methods);

  if (possibly_relocatable_methods.size() > 0) {
    // Before we actually relocate methods, we need to make sure that any
    // direct methods that they invoke are getting relocated themselves.
    // We do this by relocating one frontier of possibly relocatable methods
    // after another.
    std::unordered_map<DexMethod*, DexClass*> relocated_methods;
    size_t previous_relocated_methods_size;
    do {
      previous_relocated_methods_size = relocated_methods.size();
      for (DexMethod* m : possibly_relocatable_methods) {
        always_assert(!relocated_methods.count(m));
        if (!handle_invoked_direct_methods_that_prevent_relocation(
                m, relocated_methods)) {
          continue;
        }

        // The kind indicates the original state of a method before it was made
        // static as part of the relocation.
        RelocatedMethodKind kind;
        if (is_static(m)) {
          kind = RelocatedMethodKind::Static;
        } else {
          kind = m->is_virtual() ? RelocatedMethodKind::Virtual
                                 : RelocatedMethodKind::NonStaticDirect;
          mutators::make_static(m, mutators::KeepThis::Yes);
          m_relocated_non_static_methods.insert(m);
        }

        std::string new_type_name = create_new_type_name(kind);
        TRACE(IDEX, 3, "[dex ordering] relocating {%s::%s} to {%s::%s}\n",
              m->get_class()->get_name()->c_str(), m->get_name()->c_str(),
              new_type_name.c_str(), m->get_name()->c_str());

        DexType* new_type = DexType::make_type(new_type_name.c_str());
        ClassCreator cc(new_type);
        cc.set_access(ACC_PUBLIC | ACC_FINAL);
        cc.set_super(get_object_type());
        DexClass* relocated_cls = cc.create();
        relocated_cls->rstate.set_generated();

        int api_level = api::LevelChecker::get_method_level(m);
        relocate_method(m, new_type);

        m_relocated_method_infos.insert(
            {relocated_cls, {kind, m, cls, api_level}});
        relocated_methods.emplace(m, relocated_cls);
        relocated_classes.push_back(relocated_cls);
      }
      possibly_relocatable_methods.erase(
          std::remove_if(possibly_relocatable_methods.begin(),
                         possibly_relocatable_methods.end(),
                         [&relocated_methods](DexMethod* m) {
                           return relocated_methods.count(m);
                         }),
          possibly_relocatable_methods.end());
    } while (previous_relocated_methods_size < relocated_methods.size());
  }
}

void CrossDexRelocator::current_dex_overflowed() {
  m_classes_in_current_dex.clear();
  m_relocated_target_classes.clear();
  m_source_class_to_relocated_method_infos_map.clear();
}

void CrossDexRelocator::re_relocate_method(const RelocatedMethodInfo& info,
                                           DexClass* target_class) {
  DexMethod* method = info.method;
  always_assert(is_static(method));
  TRACE(IDEX, 4, "[dex ordering] re-relocating {%s::%s} %sto {%s::%s}\n",
        method->get_class()->get_name()->c_str(), method->get_name()->c_str(),
        target_class == info.source_class ? "back " : "",
        target_class->get_name()->c_str(), method->get_name()->c_str());
  relocate_method(method, target_class->get_type());
  if (info.kind != RelocatedMethodKind::Static &&
      !info.is_dependent_non_static_direct &&
      target_class == info.source_class) {
    // We are undoing making the method static.
    bool make_virtual = info.kind == RelocatedMethodKind::Virtual;
    mutators::make_non_static(method, make_virtual);
    if (info.kind == RelocatedMethodKind::NonStaticDirect) {
      set_private(method);
    }
    m_relocated_non_static_methods.erase(method);
  }
}

void CrossDexRelocator::add_to_current_dex(DexClass* cls) {
  m_classes_in_current_dex.insert(cls);

  auto relocated_method_infos_it = m_relocated_method_infos.find(cls);
  if (relocated_method_infos_it == m_relocated_method_infos.end()) {
    auto it = m_source_class_to_relocated_method_infos_map.find(cls);
    if (it != m_source_class_to_relocated_method_infos_map.end()) {
      // If we already earlier added relocated methods to this dex, and only
      // later it is decided that the original source class of those relocated
      // methods also gets added to the same dex, then we re-relocate back all
      // of the methods that we earlier relocated out of that source class.

      std::vector<RelocatedMethodInfo>& infos = it->second;
      for (const auto& info : infos) {
        switch (info.kind) {
        case RelocatedMethodKind::Static:
          --m_stats.relocated_static_methods;
          break;
        case RelocatedMethodKind::NonStaticDirect:
          --m_stats.relocated_non_static_direct_methods;
          break;
        case RelocatedMethodKind::Virtual:
          --m_stats.relocated_virtual_methods;
          break;
        default:
          always_assert(false);
        }
        re_relocate_method(info, cls);
      }
      m_source_class_to_relocated_method_infos_map.erase(it);
    }

    return;
  }

  const RelocatedMethodInfo& info = relocated_method_infos_it->second;
  switch (info.kind) {
  case RelocatedMethodKind::Static:
    ++m_stats.relocatable_static_methods;
    break;
  case RelocatedMethodKind::NonStaticDirect:
    ++m_stats.relocatable_non_static_direct_methods;
    break;
  case RelocatedMethodKind::Virtual:
    ++m_stats.relocatable_virtual_methods;
    break;
  default:
    always_assert(false);
  }

  DexMethod* method = info.method;
  always_assert(method->get_class() == cls->get_type());
  if (m_classes_in_current_dex.count(info.source_class)) {
    // The source class of the relocated method already has been added to the
    // current dedx. We are going to move the relocated method back to
    // its source class, effectively undoing the relocation.
    re_relocate_method(info, info.source_class);
    m_dexes_structure.squash_empty_last_class(cls);
    return;
  }

  m_source_class_to_relocated_method_infos_map[info.source_class].push_back(
      info);
  set_public(method);
  change_visibility(method);
  switch (info.kind) {
  case RelocatedMethodKind::Static:
    ++m_stats.relocated_static_methods;
    break;
  case RelocatedMethodKind::NonStaticDirect:
    ++m_stats.relocated_non_static_direct_methods;
    break;
  case RelocatedMethodKind::Virtual:
    ++m_stats.relocated_virtual_methods;
    break;
  default:
    always_assert(false);
  }

  // For runtime performance reasons, we avoid having just one giant
  // class with a vast number of static methods. Instead, we retain
  // several classes once a certain threshold is exceeded.
  auto it = m_relocated_target_classes.find(info.api_level);
  if (it == m_relocated_target_classes.end() ||
      it->second.size >= m_config.max_relocated_methods_per_class) {
    m_relocated_target_classes[info.api_level] = {cls, 1};
    ++m_stats.classes_added_for_relocated_methods;
  } else {
    // We are going to merge the method into an already emitted
    // relocation target class, allowing us to get rid of an extra
    // relocation class.
    RelocatedTargetClassInfo& target_class_info = it->second;
    ++target_class_info.size;
    re_relocate_method(info, target_class_info.cls);
    m_dexes_structure.squash_empty_last_class(cls);
  }
}

void CrossDexRelocator::cleanup(const Scope& final_scope) {
  TRACE(IDEX, 2, "[dex ordering] %zu relocatable methods\n",
        m_relocated_method_infos.size());

  // We now rewrite all invoke-instructions as needed to reflect the fact that
  // we made some methods static as part of the relocation effort.
  walk::parallel::opcodes(
      final_scope, [](DexMethod* meth) { return true; },
      [&](DexMethod*, IRInstruction* insn) {
        auto op = insn->opcode();
        switch (op) {
        case OPCODE_INVOKE_DIRECT:
        case OPCODE_INVOKE_SUPER:
        case OPCODE_INVOKE_VIRTUAL:
          if (insn->get_method()->is_def() &&
              m_relocated_non_static_methods.count(
                  static_cast<DexMethod*>(insn->get_method()))) {
            insn->set_opcode(OPCODE_INVOKE_STATIC);
          }
          break;
        case OPCODE_INVOKE_STATIC:
        case OPCODE_INVOKE_INTERFACE:
          always_assert(!insn->get_method()->is_def() ||
                        !m_relocated_non_static_methods.count(
                            static_cast<DexMethod*>(insn->get_method())));
          break;
        default:
          break;
        }
      });
}

} // namespace interdex
