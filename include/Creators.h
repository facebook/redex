/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma once

#include "DexClass.h"
#include "DexInstruction.h"
#include "DexUtil.h"
#include "Transform.h"
#include <vector>
#include <unordered_map>

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

  /**
   * Return the type of this location.
   */
  DexType* get_type() const { return type; }

  /**
   * Return the register assigned to this Location.
   */
  int get_reg() const { return reg; }

  static Location& empty() {
    static Location empty_loc(get_void_type(), 0);
    return empty_loc;
  }

 private:
  /**
   * Size of this location.
   */
  static uint16_t loc_size(DexType* type) {
    char t = type_shorty(type);
    always_assert(type != get_void_type());
    return t == 'J' || t == 'D' ? 2 : 1;
  }

 private:
  Location(DexType* t, int pos) : type(t), reg(pos) {}

  DexType* type;
  uint16_t reg;

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
  void invoke(DexMethod* meth, std::vector<Location>& args);

  /**
   * Emit the given invoke opcode to the given method with the given arguments.
   * The method can be a method ref.
   * This function can be used when the method to invoke is unknown to redex.
   * As such the proper invoke opcode cannot be determined by the DexMethod
   * given we have no access flags.
   * At some point, when importing external dependency, we could remove this
   * function as all references would be known.
   */
  void invoke(DexOpcode opcode,
              DexMethod* meth,
              std::vector<Location>& args);

  /**
   * Instance field getter.
   * The field must be a field def.
   */
  void iget(DexField* field, Location obj, Location& dst);

  /**
   * Instance field setter.
   * The field must be a field def.
   */
  void iput(DexField* field, Location self, Location src);

  /**
   * Instance field opcode.
   * The field can be a field ref.
   * This function can be used when the field is unknown to redex.
   */
  void ifield_op(DexOpcode opcode,
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
  void sfield_op(DexOpcode opcode,
                 DexField* field,
                 Location& src_or_dst);

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
   * Return the given location.
   */
  void ret(Location loc);

  /**
   * Return void.
   */
  void ret_void();

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
  MethodBlock* if_test(DexOpcode if_op,
                       Location first,
                       Location second);

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
  MethodBlock* if_testz(DexOpcode if_op, Location test);

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
  MethodBlock* if_else_test(DexOpcode if_op,
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
  MethodBlock* if_else_testz(DexOpcode if_op,
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

 private:
  MethodBlock(FatMethod::iterator iterator, MethodCreator* creator);

  //
  // Helpers
  //

  void push_instruction(DexInstruction* insn);
  MethodBlock* make_if_block(DexInstruction* insn);
  MethodBlock* make_if_else_block(DexInstruction* insn, MethodBlock** true_block);
  MethodBlock* make_switch_block(DexInstruction* insn,
                                 std::map<int, MethodBlock*>& cases);

 private:
  MethodCreator* mc;
  // A MethodBlock is simply an iterator over a FatMethod used to emit
  // instructions
  FatMethod::iterator curr;

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
  MethodCreator(DexType* cls,
                DexString* name,
                DexProto* proto,
                DexAccessFlags access);

  /**
   * Get an existing local.
   */
  Location& get_local(int i) {
    always_assert(i < static_cast<int>(locals.size()));
    return locals.at(i);
  }

  /**
   * Make a new local of the given type.
   */
  Location& make_local(DexType* type) {
    Location local{type, top_reg};
    locals.push_back(std::move(local));
    top_reg += Location::loc_size(type);
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
   * On return the DexCode of the method in input is null'ed.
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

  /**
   * Forward a method to another method.
   * This will delete the current DexCode if the method had one.
   * It is usually used together with make_static_from() in order to
   * forward the "promoted" method to the static method.
   * Methods must be signature compatible.
   */
  static void forward_method_to(DexMethod* meth, DexMethod* smeth);

 private:
  //
  // Helpers
  //

  std::unique_ptr<DexCode>& to_code();
  void load_locals(DexMethod* meth);
  uint16_t ins_count() const;

  uint16_t get_real_reg_num(uint16_t vreg) {
    if (vreg < ins_count()) {
      return static_cast<uint16_t>(top_reg - ins_count() + vreg);
    }
    return top_reg - vreg - 1;
  }

  FatMethod::iterator push_instruction(FatMethod::iterator curr, DexInstruction* insn);
  FatMethod::iterator make_if_block(FatMethod::iterator curr,
                                    DexInstruction* insn,
                                    FatMethod::iterator* false_block);
  FatMethod::iterator make_if_else_block(FatMethod::iterator curr,
                                         DexInstruction* insn,
                                         FatMethod::iterator* false_block,
                                         FatMethod::iterator* true_block);
  FatMethod::iterator make_switch_block(
      FatMethod::iterator curr,
      DexInstruction* opcode,
      FatMethod::iterator* default_block,
      std::map<int, FatMethod::iterator>& cases);

 private:
  DexMethod* method;
  MethodTransform* meth_code;
  uint16_t out_count;
  uint16_t top_reg;
  DexAccessFlags access;
  std::vector<Location> locals;
  MethodBlock* main_block;

  friend std::string show(const MethodCreator*);
  friend struct MethodBlock;
};

/**
 * Create a DexClass.
 * Once create is called this creator should not be used any longer.
 */
struct ClassCreator {
  explicit ClassCreator(DexType* type) {
    always_assert_log(type_class(type) == nullptr,
        "class already exists for %s\n", SHOW(type));
    m_cls = new DexClass();
    m_cls->m_self = type;
    m_cls->m_access_flags = (DexAccessFlags)0;
    m_cls->m_super_class = nullptr;
    m_cls->m_interfaces = nullptr;
    m_cls->m_source_file = nullptr;
    m_cls->m_anno = nullptr;
    m_cls->m_has_class_data = false;
    m_cls->m_external = false;
  }

  /**
   * Return the DexClass associated with this creator.
   */
  DexClass* get_class() const {
    return m_cls;
  }

  /**
   * Return the DexType associated with this creator.
   */
  DexType* get_type() const {
    return m_cls->get_type();
  }

  /**
   * Accessibility flags
   */
  DexAccessFlags get_access() const { return m_cls->get_access(); }

  /**
   * Set the parent of the DexClass to be created.
   */
  void set_super(DexType* super) {
    m_cls->m_super_class = super;
  }

  /**
   * Set the access flags for the DexClass to be created.
   */
  void set_access(DexAccessFlags access) {
    m_cls->m_access_flags = access;
  }

  /**
   * Set the external bit for the DexClass.
   */
  void set_external() {
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
  void add_field(DexField* field) {
    m_cls->add_field(field);
  }

  /**
   * Add a DexMethod to the DexClass.
   */
  void add_method(DexMethod* method) {
    m_cls->add_method(method);
  }

  /**
   * Create the DexClass. The creator should not be used after this call.
   */
  DexClass* create() {
    always_assert_log(m_cls->m_self,
                      "Self cannot be null in a DexClass");
    if (m_cls->m_super_class == NULL) {
      if(m_cls->m_self != get_object_type()) {
        always_assert_log(m_cls->m_super_class,
                          "No supertype found for %s", SHOW(m_cls->m_self));
      }
    }
    m_cls->m_has_class_data = true;
    m_cls->m_interfaces = DexTypeList::make_type_list(std::move(m_interfaces));
    build_type_system(m_cls);
    return m_cls;
  }

private:
  DexClass* m_cls;
  std::list<DexType*> m_interfaces;
};
