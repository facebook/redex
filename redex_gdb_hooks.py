# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# Defines GDB Pretty printing support for Redex
import gdb


class CallableBase(object):
    def __init__(self, val, cmd, reftype):
        self.val = val
        self.cmd = cmd
        self.reftype = reftype

    def to_string(self):
        if self.reftype and int(self.val) == 0:
            return "NULL"
        try:
            res = gdb.execute(self.cmd, False, True)
            # Print instead of returning the string to handle newlines
            prncmd = 'call printf ("%s", "{0}\\n")'.format(
                res.rstrip().replace('"', "")
            )
            gdb.execute(prncmd, False, True)
            return ""
        except BaseException:
            return ""


def Show(CallableBase):
    def printer(val):
        cmd = "call('show({0} const)'({1}))".format(type, val)
        return CallableBase(val, cmd, False)

    return printer


def ShowDeref(type):
    def printer(val):
        cmd = "call('show({0} const*)'({1}))".format(type, hex(val))
        return CallableBase(val, cmd, True)

    return printer


def ShowDerefAsRef(type):
    def printer(val):
        cmd = "call('show({0} const&)'(*{1}))".format(type, hex(val))
        return CallableBase(val, cmd, True)

    return printer


def ShowRef(type):
    def printer(val):
        val = val.address
        cmd = "call('show({0} const&)'(*{1}))".format(type, hex(val))
        return CallableBase(val, cmd, True)

    return printer


def ShowDeobfuscated(type):
    def printer(val):
        cmd = "call('show_deobfuscated({0} const*)'({1})".format(type, hex(val))
        return CallableBase(val, cmd, True)

    return printer


pretty_printers_dict = {
    # type.unqualified() doesnt seem to remove "const" qualifier.
    "cfg::ControlFlowGraph *": ShowDerefAsRef("cfg::ControlFlowGraph"),
    "const cfg::ControlFlowGraph *": ShowDerefAsRef("cfg::ControlFlowGraph"),
    "cfg::ControlFlowGraph &": ShowRef("cfg::ControlFlowGraph"),
    "const cfg::ControlFlowGraph &": ShowRef("cfg::ControlFlowGraph"),
    "cfg::Edge *": ShowDeref("cfg::Edge"),
    "const cfg::Edge *": ShowDeref("cfg::Edge"),
    "cfg::Block *": ShowDeref("cfg::Block"),
    "const cfg::Block *": ShowDeref("cfg::Block"),
    "DexAnnotation *": ShowDeref("DexAnnotation"),
    "const DexAnnotation *": ShowDeref("DexAnnotation"),
    "DexAnnotationDirectory *": ShowDeref("DexAnnotationDirectory"),
    "const DexAnnotationDirectory *": ShowDeref("DexAnnotationDirectory"),
    "DexAnnotationSet *": ShowDeref("DexAnnotationSet"),
    "const DexAnnotationSet *": ShowDeref("DexAnnotationSet"),
    "DexCode *": ShowDeref("DexCode"),
    "const DexCode *": ShowDeref("DexCode"),
    "DexFieldRef *": ShowDeref("DexFieldRef"),
    "cost DexFieldRef *": ShowDeref("DexFieldRef"),
    "DexField *": ShowDeref("DexField"),
    "const DexField *": ShowDeref("DexField"),
    "DexInstruction *": ShowDeref("DexInstruction"),
    "const DexInstruction *": ShowDeref("DexInstruction"),
    "DexOpcode": Show("DexOpcode"),
    "const DexOpcode": Show("DexOpcode"),
    "DexProto *": ShowDeref("DexProto"),
    "const DexProto *": ShowDeref("DexProto"),
    "DexString *": ShowDeref("DexString"),
    "const DexString *": ShowDeref("DexString"),
    "DexClass *": ShowDeref("DexClass"),
    "const DexClass *": ShowDeref("DexClass"),
    "DexMethodRef *": ShowDeref("DexMethodRef"),
    "const DexMethodRef *": ShowDeref("DexMethodRef"),
    "DexMethod *": ShowDeref("DexMethod"),
    "const DexMethod *": ShowDeref("DexMethod"),
    "DexType *": ShowDeref("DexType"),
    "const DexType *": ShowDeref("DexType"),
    "DexTypeList *": ShowDeref("DexTypeList"),
    "const DexTypeList *": ShowDeref("DexTypeList"),
    "IRInstruction *": ShowDeref("IRInstruction"),
    "const IRInstruction *": ShowDeref("IRInstruction"),
    # TODO printing IRCode seems to crash gdb
    # "IRCode *": ShowDeref("IRCode"),
    # "const IRCode *": ShowDeref("IRCode"),
    "IRList *": ShowDeref("IRList"),
    "const IRList *": ShowDeref("IRList"),
    "IROpcode": Show("IROpcode"),
    "const IROpcode": Show("IROpcode"),
}


def lookup_function(val):
    if val is None or val.type is None:
        return None
    type = val.type
    type_name = str(type.unqualified().strip_typedefs())
    if type_name in pretty_printers_dict:
        return pretty_printers_dict[type_name](val)
    return None


def register_pretty_printer(obj):
    if obj is None:
        obj = gdb
    obj.pretty_printers.insert(0, lookup_function)


def get_gdb_val_for_str(arg):
    val = gdb.lookup_symbol(arg)
    if not (val[0] is None):
        frame = gdb.selected_frame()
        return val[0].value(frame)
    val = gdb.lookup_global_symbol(arg)
    if not (val is None):
        return val.value()
    val = gdb.lookup_static_symbol(arg)
    if not (val is None):
        return val.value()
    return None


class pp(gdb.Command):
    def __init__(self):
        gdb.Command.__init__(self, "pp", gdb.COMMAND_DATA, gdb.COMPLETE_SYMBOL, True)

    def invoke(self, arg, from_tty):
        val = get_gdb_val_for_str(arg)
        printer = lookup_function(val)
        if printer is None:
            print(('No symbol "{0}" in current context'.format(arg)))
            return
        printer.to_string()


pp()
# register_pretty_printer(gdb.current_objfile())
print("Redex pretty printers added.")
print("Use custom command pp to print Redex symbols")
