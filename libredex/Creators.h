/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <unordered_map>
#include <vector>

#include "DexClass.h"
#include "DexUtil.h"
#include "IRCode.h"
#include "IRInstruction.h"

struct MethodCreator;
struct MethodBlock;

/**
 * A Location holds a register and a type.
 * Operations to generate code are based on Locations.
 * The register is an implementation detail and should not be used.
 * The type may be analyzed in some situation.
 */
struct Location {
  /**
   * Whether a Location is compatible with the given type.
   * Compatibility is only defined in terms of size right now.
   * Wide locations (double and long) hold 2 registers and should be used
   * with type of the same kind.
   */
  bool is_compatible(DexType* t) const { return loc_size(type) == loc_size(t); }

  /**
   * Whether the location is wide.
   */
  bool is_wide() const { return loc_size(type) == 2; }

  bool is_ref() const {
    char t = type::type_shorty(type);
    return t == 'L' || t == '[';
  }

  /**
   * Return the type of this location.
   */
  DexType* get_type() const { return type; }

  /**
   * Return the register assigned to this Location.
   */
  int get_reg() const { return reg; }

  static Location& empty() {
    static Location empty_loc(type::_void(), 0);
    return empty_loc;
  }

  Location(const Location&) = default;
  Location(Location&&) noexcept = default;

  Location& operator=(const Location&) = default;
  Location& operator=(Location&&) = default;

 private:
  /**
   * Size of this location.
   */
  static uint16_t loc_size(DexType* type) {
    char t = type::type_shorty(type);
    always_assert(type != type::_void());
    return t == 'J' || t == 'D' ? 2 : 1;
  }

 private:
  Location(DexType* t, reg_t pos) : type(t), reg(pos) {}

  DexType* type;
  reg_t reg;

  friend struct MethodBlock;
  friend struct MethodCreator;
};

/**
 * A MethodBlock is the single object used to emit code.
 * Unlike high level languages a block here can only be introduced by
 * instructions that would cause a jump (if/else, switch, etc.).
 * A MethodBlock hides the details of jumping instructions and no offset or
 * goto has to be emitted when working exclusively with MethodBlocks.
 * Code can be emitted in any block at any time.
 */
struct MethodBlock {
  //
  // member instructions
  //

  /**
   * Emit an invoke* opcode to the given method with the given arguments.
   * The method must be a method def.
   */
  void invoke(DexMethod* meth, const std::vector<Location>& args);

  /**
   * Emit the given invoke opcode to the given method with the given arguments.
   * The method can be a method ref.
   * This function can be used when the method to invoke is unknown to redex.
   * As such the proper invoke opcode cannot be determined by the DexMethod
   * given we have no access flags.
   * At some point, when importing external dependency, we could remove this
   * function as all references would be known.
   */
  void invoke(IROpcode opcode,
              DexMethodRef* meth,
              const std::vector<Location>& args);

  /**
   * new-instance; instatiate 'type' into dst location.
   */
  void new_instance(DexType* type, Location& dst);

  /**
   * new-array; instatiate an array 'type' of 'size' into dst location.
   */
  void new_array(DexType* type, const Location& size, const Location& dst);

  /**
   * throw; throw ex object at Location
   */
  void throwex(Location ex);

  /**
   * Instance field getter.
   * The field must be a field def.
   */
  void iget(DexField* field, Location obj, Location& dst);

  /**
   * Instance field setter.
   * The field must be a field def.
   */
  void iput(DexField* field, Location obj, Location src);

  /**
   * Instance field opcode.
   * The field can be a field ref.
   * This function can be used when the field is unknown to redex.
   */
  void ifield_op(IROpcode opcode,
                 DexField* field,
                 Location obj,
                 Location& src_or_dst);

  /**
   * Static field getter.
   * The field must be a field def.
   */
  void sget(DexField* field, Location& dst);

  /**
   * Static field setter.
   * The field must be a field def.
   */
  void sput(DexField* field, Location src);

  /**
   * Static field opcode.
   * The field can be a field ref.
   * This function can be used when the field is unknown to redex.
   */
  void sfield_op(IROpcode opcode, DexField* field, Location& src_or_dst);

  //
  // simple instruction (location based)
  //

  /**
   * Move the src location into the dst location.
   */
  void move(Location src, Location& dst);

  /**
   * Move the result of an invoke into the given Location.
   * Changes the Location to have the given type.
   */
  void move_result(Location& dst, DexType* type);

  /**
   * Check-cast a location to a given type.
   */
  void check_cast(Location& src_and_dst, DexType* type);

  void instance_of(Location& obj, Location& dst, DexType* type);

  /**
   * Return the given location.
   */
  void ret(Location loc);

  /**
   * Return void.
   */
  void ret_void();

  /**
   * Return the given location based on its type.
   */
  void ret(DexType* rtype, Location loc);

  /**
   * Load an int32 constant into the given Location.
   * The Location must be compatible with the given type.
   */
  void load_const(Location& loc, int32_t value);

  /**
   * Load the double value into the given Location.
   * The Location must be compatible with the given type.
   */
  void load_const(Location& loc, double value);

  /**
   * Load the string value into the given Location.
   * The Location must be compatible with the given type.
   */
  void load_const(Location& loc, DexString* value);

  /**
   * Load the type value into the given Location.
   * The Location must be compatible with the given type.
   */
  void load_const(Location& loc, DexType* value);

  /**
   * Load null into the given Location.
   */
  void load_null(Location& loc);

  // Helper
  void init_loc(Location& loc);

  void binop_lit16(IROpcode op,
                   const Location& dest,
                   const Location& src,
                   int16_t literal);
  void binop_lit8(IROpcode op,
                  const Location& dest,
                  const Location& src,
                  int8_t literal);

  //
  // branch instruction
  //

  /**
   * Emit an if* opcode that tests 2 Locations.
   * It returns the MethodBlock that would be executed if the condition fails
   * (false block).
   * No goto instruction or offset need to be set.
   * if_<op> x, y goto label
   *   code1     <-- returned block points here
   *   code2
   * label:
   *   code3     <-- block on which the function was called points here
   *   code4
   *   ret
   */
  MethodBlock* if_test(IROpcode if_op, Location first, Location second);

  /**
   * Emit an if* opcode that tests a Location against 0.
   * It returns the MethodBlock that would be executed if the condition fails
   * (false block).
   * No goto instruction or offset need to be set.
   * if_<op> x goto label
   *   code1     <-- returned block points here
   *   code2
   * label:
   *   code3     <-- block on which the function was called points here
   *   code4
   *   ret
   */
  MethodBlock* if_testz(IROpcode if_op, Location test);

  /**
   * Emit an if* opcode that tests 2 Locations.
   * It returns the MethodBlock that would be executed if the condition fails.
   * The out else_block is where the code jumps if the condition holds.
   * No goto instruction or offset need to be set.
   * if_<op> x, y goto else_label
   *   code1     <-- returned block points here
   *   code2
   * end_if_label:
   *   code3     <-- block on which the function was called points here
   *   code4
   *   ret
   * else_label:
   *   code3     <-- else_block points here
   *   code4
   *   got end_if_label // emitted automatically
   */
  MethodBlock* if_else_test(IROpcode if_op,
                            Location first,
                            Location second,
                            MethodBlock** true_block);

  /**
   * Emit an if* opcode that tests a Location against 0.
   * It returns the MethodBlock that would be executed if the condition fails.
   * The out else_block is where the code jumps if the condition holds.
   * No goto instruction or offset need to be set.
   * if_<op> x goto else_label
   *   code1     <-- returned block points here
   *   code2
   * end_if_label:
   *   code3     <-- block on which the function was called points here
   *   code4
   *   ret
   * else_label:
   *   code3     <-- else_block points here
   *   code4
   *   got end_if_label // emitted automatically
   */
  MethodBlock* if_else_testz(IROpcode if_op,
                             Location test,
                             MethodBlock** true_block);

  /**
   * Emit an switch opcode against the test Locations.
   * It returns the MethodBlock for the default case.
   * On return the std::map will contain MethodBlock for each case.
   * switch_op cond jump case1, case2,...
   *   code1     <-- default case block
   *   code2
   * end_switch_label:
   *   code3     <-- block on which the function was called points here
   *   code4
   *   ret
   * case1:
   *   code5
   *   goto end_switch_label // emitted automatically
   * case2:
   *   code6
   *   goto end_switch_label // emitted automatically
   */
  MethodBlock* switch_op(Location test, std::map<int, MethodBlock*>& cases);
  MethodBlock* switch_op(Location test,
                         std::map<SwitchIndices, MethodBlock*>& cases);

 private:
  MethodBlock(const IRList::iterator& iterator, MethodCreator* creator);

  //
  // Helpers
  //

  void push_instruction(IRInstruction* insn);
  MethodBlock* make_if_block(IRInstruction* insn);
  MethodBlock* make_if_else_block(IRInstruction* insn,
                                  MethodBlock** true_block);
  MethodBlock* make_switch_block(IRInstruction* insn,
                                 std::map<SwitchIndices, MethodBlock*>& cases);

 private:
  MethodCreator* mc;
  // A MethodBlock is simply an iterator over an IRList used to emit
  // instructions
  IRList::iterator curr;

  friend struct MethodCreator;
  friend std::string show(const MethodBlock*);
};

/**
 * Main class to create methods.
 * This class is responsible for locals and the main block.
 * Locals are "global" to the method. There are no block scoped locals and
 * it's not clear there will ever be.
 * Locals can be made as needed and according to type compatibility.
 * Locals go from 0 to n where 0 is the first argument to the function emitted
 * and so forth.
 */
struct MethodCreator {
 public:
  explicit MethodCreator(DexMethod* meth);
  MethodCreator(DexMethodRef* ref,
                DexAccessFlags access,
                DexAnnotationSet* anno = nullptr,
                bool with_debug_item = false);
  MethodCreator(DexType* cls,
                DexString* name,
                DexProto* proto,
                DexAccessFlags access,
                DexAnnotationSet* anno = nullptr,
                bool with_debug_item = false);

  /**
   * Get an existing local.
   */
  Location get_local(int i) {
    always_assert(i < static_cast<int>(locals.size()));
    return locals.at(i);
  }

  std::vector<Location> get_reg_args();

  /**
   * Make a new local of the given type.
   */
  Location make_local(DexType* type) {
    auto next_reg = meth_code->get_registers_size();
    locals.emplace_back(Location{type, next_reg});
    meth_code->set_registers_size(next_reg + Location::loc_size(type));
    return locals.back();
  }

  /**
   * Return the main block to be used to emit code.
   */
  MethodBlock* get_main_block() const { return main_block; }

  /**
   * Return the newly created method.
   */
  DexMethod* create();

 public:
  /**
   * Transfer code from a given method to a static with the same signature
   * in the given class.
   * This can be used to "promote" instance methods to static.
   * On return the IRCode of the method in input is null'ed.
   * Essentially ownership of the code is passed to the generated static.
   */
  static DexMethod* make_static_from(DexMethod* meth, DexClass* target_cls);

  /**
   * Same as make_static_from(DexMethod*, DexClass*); but create a method
   * with the given name.
   */
  static DexMethod* make_static_from(DexString* name,
                                     DexMethod* meth,
                                     DexClass* target_cls);

  /**
   * Same as make_static_from(DexMethod*, DexClass*); but create a method
   * with the given name and the given proto.
   * The proto provided must be compatible with the code in meth.
   */
  static DexMethod* make_static_from(DexString* name,
                                     DexProto* proto,
                                     DexMethod* meth,
                                     DexClass* target_cls);

 private:
  //
  // Helpers
  //

  void load_locals(DexMethod* meth);

  Location make_local_at(DexType* type, reg_t i) {
    always_assert(i < meth_code->get_registers_size());
    locals.emplace_back(Location{type, i});
    return locals.back();
  }

  IRList::iterator push_instruction(const IRList::iterator& curr,
                                    IRInstruction* insn);
  IRList::iterator make_if_block(IRList::iterator curr,
                                 IRInstruction* insn,
                                 IRList::iterator* false_block);
  IRList::iterator make_if_else_block(IRList::iterator curr,
                                      IRInstruction* insn,
                                      IRList::iterator* false_block,
                                      IRList::iterator* true_block);
  IRList::iterator make_switch_block(
      IRList::iterator curr,
      IRInstruction* insn,
      IRList::iterator* default_block,
      std::map<SwitchIndices, IRList::iterator>& cases);

 private:
  DexMethod* method;
  IRCode* meth_code;
  std::vector<Location> locals;
  std::vector<MethodBlock*> blocks;
  MethodBlock* main_block;
  bool m_with_debug_item;

  friend std::string show(const MethodCreator*);
  friend struct MethodBlock;
};

/**
 * Create a DexClass.
 * Once create is called this creator should not be used any longer.
 */
struct ClassCreator {
  explicit ClassCreator(DexType* type, const std::string& location = "") {
    always_assert_log(type_class(type) == nullptr,
                      "class already exists for %s\n", show_type(type).c_str());
    m_cls = new DexClass(location);
    m_cls->m_self = type;
    m_cls->m_access_flags = (DexAccessFlags)0;
    m_cls->m_super_class = nullptr;
    m_cls->m_interfaces = DexTypeList::make_type_list({});
    m_cls->m_source_file = nullptr;
    m_cls->m_anno = nullptr;
    m_cls->m_external = false;
    m_cls->m_perf_sensitive = false;
    m_cls->set_deobfuscated_name(type->get_name()->c_str());
  }

  /**
   * Return the DexClass associated with this creator.
   */
  DexClass* get_class() const { return m_cls; }

  /**
   * Return the DexType associated with this creator.
   */
  DexType* get_type() const { return m_cls->get_type(); }

  /**
   * Accessibility flags
   */
  DexAccessFlags get_access() const { return m_cls->get_access(); }

  /**
   * Set the parent of the DexClass to be created.
   */
  void set_super(DexType* super) { m_cls->m_super_class = super; }

  /**
   * Set the access flags for the DexClass to be created.
   */
  void set_access(DexAccessFlags access) { m_cls->m_access_flags = access; }

  /**
   * Set the external bit for the DexClass.
   */
  void set_external() {
    m_cls->m_deobfuscated_name = show_cls(m_cls);
    m_cls->m_external = true;
  }

  /**
   * Add an interface to the DexClass to be created.
   */
  void add_interface(DexType* intf) {
    if (std::find(m_interfaces.begin(), m_interfaces.end(), intf) ==
        m_interfaces.end()) {
      m_interfaces.push_back(intf);
    }
  }

  /**
   * Add a DexField to the DexClass.
   */
  void add_field(DexField* field) { m_cls->add_field(field); }

  /**
   * Add a DexMethod to the DexClass.
   */
  void add_method(DexMethod* method) { m_cls->add_method(method); }

  /**
   * Create the DexClass. The creator should not be used after this call.
   */
  DexClass* create() {
    always_assert_log(m_cls->m_self, "Self cannot be null in a DexClass");
    if (m_cls->m_super_class == NULL) {
      if (m_cls->m_self != type::java_lang_Object()) {
        always_assert_log(m_cls->m_super_class, "No supertype found for %s",
                          show_type(m_cls->m_self).c_str());
      }
    }
    m_cls->m_interfaces = DexTypeList::make_type_list(std::move(m_interfaces));
    g_redex->publish_class(m_cls);
    return m_cls;
  }

 private:
  // To avoid "Show.h" in the header.
  static std::string show_cls(const DexClass* cls);
  static std::string show_type(const DexType* type);

  DexClass* m_cls;
  std::deque<DexType*> m_interfaces;
};
