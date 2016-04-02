%{
/*
 * This is a subset of the proguard class specification language.
 * Reference: http://proguard.sourceforge.net/index.html#manual/examples.html
 */


#define YYSTYPE char*

#include <iostream>
#include <cstdio>
#include <cstdlib>
#include "configparser/keeprules.h"

extern "C" int yylex();
extern "C" int yyparse();
extern "C" FILE *yyin;
extern "C" int line_number;

#define WARN(x) printf("WARNING: %s\n", x)
#define ASSERT(x, y) if (!(x)) { printf("ERROR: %s", y); exit(-1);}

void yyerror(const char* msg);

std::vector<KeepRule>* rules = nullptr;

// Used for both classes and members
uint32_t flags;

// Params for keep rule
int class_type;
const char* classname;
const char* extends;
bool allow_deletion;

static KeepRule* keeprule;

void keep_rule_start() {
     ASSERT(keeprule == nullptr, "keeprule should have been nulled out and deleted by keep_rule_end()");
     keeprule = new KeepRule();
}

void keep_rule_end() {
    if (!rules) {
        rules = new std::vector<KeepRule>();
    }
    rules->push_back(*keeprule);
    delete keeprule;
    keeprule = nullptr;
}


// Params for member
const char* member_annotation = nullptr;
const char* member_type = nullptr;
const char* member_name = nullptr;
bool member_is_method;
MethodFilter* method_filter = nullptr;


void member_start() {
    flags = 0;
}

void member_args_start() {
    if (!keeprule) {
        return;
    }
    MethodFilter method(flags, member_name, member_type);
    keeprule->methods.push_back(method);
    // Keep a pointer to it so we can add param definitions
    method_filter = &(keeprule->methods[keeprule->methods.size() - 1]);
}

void member_args_end() {
    method_filter = nullptr;
}

void member_end() {
    if (!keeprule) {
        return;
    }
    if (member_is_method) {
        // Method filter has already been added to keep rule.
        method_filter = nullptr;
    } else {
        FieldFilter field(flags, member_annotation, member_name, member_type);
        keeprule->fields.push_back(field);
    }
}

static char* duplicate(char* original) {
  if (original == nullptr) {
    return nullptr;
  }
  char* newptr = new char[strlen(original) + 1];
  strcpy(newptr, original);
  return newptr;
}

%}


%error-verbose

%token T_NEWLINE
%token T_SEMICOLON
%token T_COMMA
%token T_NOT
%token T_AT

%token T_COMMENT

%token T_KEEP  /* Keep class and members from being deleted or renamed */
%token T_KEEPNAMES /* Keep class and members from being renamed */
%token T_KEEPCLASSMEMBERS /* Keep class members from being deleted and renamed */
%token T_KEEPCLASSMEMBERNAMES /* Keep class members from being renamed */
%token T_KEEPCLASSESWITHMEMBERS /* Don't delete or rename if specified members present */
%token T_KEEPCLASSESWITHMEMBERNAMES /* Don't rename if specified members present */

%token T_ALLOWOBFUSCATION
%token T_ALLOWOPTIMIZATION
%token T_ALLOWSHRINKING

/* Unsupported proguard config rules */
%token T_ADAPTCLASSSTRINGS
%token T_ADAPTRESOURCEFILECONTENTS
%token T_ADAPTRESOURCEFILENAMES
%token T_ALLOWACCESSMODIFICATION
%token T_APPLYMAPPING
%token T_ASSUMENOSIDEEFFECTS
%token T_CLASSOBFUSCATIONDICTIONARY
%token T_DONTOBFUSCATE
%token T_DONTOPTIMIZE
%token T_DONTPREVERIFY
%token T_DONTSHRINK
%token T_DONTWARN
%token T_DONTUSEMIXEDCASECLASSNAMES
%token T_FLATTENPACKAGEHIERARCHY
%token T_KEEPATTRIBUTES
%token T_KEEPPACKAGENAMES
%token T_KEEPPARAMETERNAMES
%token T_MERGEINTERFACESAGGRESSIVELY
%token T_OBFUSCATIONDICTIONARY
%token T_OPTIMIZATIONPASSES
%token T_OPTIMIZATIONS
%token T_OVERLOADAGGRESSIVELY
%token T_PACKAGEOBFUSCATIONDICTIONARY
%token T_PRINTMAPPING
%token T_PRINTSEEDS
%token T_PRINTUSAGE
%token T_RENAMESOURCEFILEATTRIBUTE
%token T_REPACKAGECLASSES
%token T_USEUNIQUECLASSMEMBERNAMES
%token T_WHYAREYOUKEEPING

%token T_CLASS T_ENUM T_INTERFACE T_AT_INTERFACE
%token T_INIT
%token T_IMPLEMENTS T_EXTENDS
%token T_PUBLIC T_PRIVATE T_PROTECTED
%token T_STATIC T_FINAL T_TRANSIENT T_NATIVE


%token T_METHODS T_FIELDS T_ANY_MEMBER

%token T_PATTERN
%token T_MEMBERS_BEGIN T_MEMBERS_END
%token T_ARGS_BEGIN T_ARGS_END

%%

START:
    RULE_LIST;

RULE_LIST:
    /* Note: RULE RULE_LIST is much less efficient */
    RULE_LIST RULE |
    RULE;

RULE:
    T_COMMENT |
    UNSUPPORTED_PROGUARD_RULE |
    KEEP_RULE;

KEEP_RULE:
    {keep_rule_start();}
    KEEP_TYPE KEEP_MODIFIERS CLASS_FILTER CLASS_MEMBERS
    {keep_rule_end();};

KEEP_TYPE:
    T_KEEP                       {keeprule->allow_deletion = false;
      keeprule->allow_cls_rename = true; keeprule->allow_member_rename = true;} |
    T_KEEPNAMES                  {keeprule->allow_deletion = true;
      keeprule->allow_cls_rename = false; keeprule->allow_member_rename = false;}  |
    T_KEEPCLASSMEMBERS           {keeprule->allow_deletion = false;
      keeprule->allow_cls_rename = true; keeprule->allow_member_rename = true;} |
    T_KEEPCLASSMEMBERNAMES       {keeprule->allow_deletion = true;
      keeprule->allow_cls_rename = true; keeprule->allow_member_rename = false;}  |
    T_KEEPCLASSESWITHMEMBERS     {keeprule->allow_deletion = false;
      keeprule->allow_cls_rename = false; keeprule->allow_member_rename = false;} |
    T_KEEPCLASSESWITHMEMBERNAMES {keeprule->allow_deletion = true;
      keeprule->allow_cls_rename = true; keeprule->allow_member_rename = false;};

KEEP_MODIFIERS:
    /* empty */ |
    T_COMMA ALLOWED_OPERATION KEEP_MODIFIERS;

ALLOWED_OPERATION:
    T_ALLOWOBFUSCATION  {WARN("'allowobfuscation' is not supported.\n"); }|
    T_ALLOWOPTIMIZATION {WARN("'allowoptimization' is not supported.\n");}|
    T_ALLOWSHRINKING    {keeprule->allow_deletion = true;};

CLASS_FILTER:
    {flags = 0;} ANNOTATION VISIBILITY ATTRIBUTES CLASS_SPEC IMPLEMENTS_OR_EXTENDS {if (keeprule) keeprule->flags = flags;}

CLASS_SPEC:
    CLASS_TYPE CLASS_NAME;

CLASS_TYPE:
    /* empty */  {if (keeprule) keeprule->class_type = keeprules::ANY_CLASS_TYPE;} |
    T_CLASS        {if (keeprule) keeprule->class_type = keeprules::CLASS;}          |
    T_ENUM         {if (keeprule) keeprule->class_type = keeprules::ENUMERATION;}    |
    T_INTERFACE    {if (keeprule) keeprule->class_type = keeprules::INTERFACE;}      |
    T_AT_INTERFACE {if (keeprule) keeprule->class_type = keeprules::ANNOTATION;} ;

CLASS_NAME:
    T_PATTERN {if (keeprule) keeprule->classname = duplicate(yylval);};

IMPLEMENTS_OR_EXTENDS:
    /* empty */        {if (keeprule) keeprule->extends = nullptr;} |
    T_IMPLEMENTS T_PATTERN {if (keeprule) keeprule->extends = duplicate(yylval);} |
    T_EXTENDS T_PATTERN    {if (keeprule) keeprule->extends = duplicate(yylval);};

CLASS_MEMBERS:
    /* empty */ |
    T_MEMBERS_BEGIN MEMBERS_LIST T_MEMBERS_END;

MEMBERS_LIST:
    MEMBERS_LIST MEMBER T_SEMICOLON |
    MEMBER T_SEMICOLON;

MEMBER:
    {member_start();}
    ANNOTATION
    VISIBILITY
    ATTRIBUTES
    MEMBER_NAME
    ARGS
    {member_end();};

ANNOTATION:
    /* empty */ |
    T_AT T_PATTERN {member_annotation = duplicate(yylval);};

VISIBILITY:
    /* empty */ |
    T_PUBLIC    {flags |= keeprules::PUBLIC;} |
    T_PRIVATE   {flags |= keeprules::PRIVATE;}|
    T_PROTECTED {flags |= keeprules::PROTECTED;};

ATTRIBUTES:
    /* empty */ |
    ATTRIBUTE_TERM ATTRIBUTES;

ATTRIBUTE_TERM:
    T_NOT ATTRIBUTE |
    ATTRIBUTE;

ATTRIBUTE:
    T_STATIC     {flags |= keeprules::STATIC;} |
    T_FINAL      {flags |= keeprules::FINAL;} |
    T_TRANSIENT  {flags |= keeprules::TRANSIENT;} |
    T_NATIVE     {flags |= keeprules::NATIVE;};

MEMBER_NAME:
    T_INIT |
    T_FIELDS |
    T_METHODS |
    T_ANY_MEMBER {member_type = "*"; member_name = "*";} |
    T_PATTERN {member_type = duplicate(yylval);} T_PATTERN {member_name = duplicate(yylval);}
    ;

ARGS:
    /* empty -- indicates that this member was a field */ |
    T_ARGS_BEGIN {member_args_start(); member_args_end();/* Method that takes no args */} T_ARGS_END |
    T_ARGS_BEGIN {member_args_start(); /* Method with args */} ARGS_LIST {member_args_end();} T_ARGS_END;

ARGS_LIST:
    ARGS_LIST T_COMMA ARG |
    ARG;

ARG:
    T_PATTERN {if (method_filter) { method_filter->params.push_back(duplicate(yylval));}} ;

UNSUPPORTED_PROGUARD_RULE:
    T_ADAPTCLASSSTRINGS T_PATTERN |
    T_ADAPTRESOURCEFILECONTENTS T_PATTERN |
    T_ADAPTRESOURCEFILENAMES T_PATTERN |
    T_ALLOWACCESSMODIFICATION |
    T_APPLYMAPPING T_PATTERN |
    T_ASSUMENOSIDEEFFECTS CLASS_FILTER CLASS_MEMBERS |
    T_CLASSOBFUSCATIONDICTIONARY T_PATTERN |
    T_DONTOBFUSCATE |
    T_DONTOPTIMIZE |
    T_DONTPREVERIFY |
    T_DONTSHRINK |
    T_DONTWARN T_PATTERN |
    T_DONTUSEMIXEDCASECLASSNAMES |
    T_FLATTENPACKAGEHIERARCHY T_PATTERN |
    T_KEEPATTRIBUTES PATTERN_LIST |
    T_KEEPPACKAGENAMES PATTERN_LIST |
    T_KEEPPARAMETERNAMES |
    T_MERGEINTERFACESAGGRESSIVELY |
    T_OBFUSCATIONDICTIONARY T_PATTERN |
    T_OPTIMIZATIONPASSES T_PATTERN |
    T_OPTIMIZATIONS OPTIMIZATION_LIST |
    T_OVERLOADAGGRESSIVELY |
    T_PACKAGEOBFUSCATIONDICTIONARY T_PATTERN |
    T_PRINTMAPPING T_PATTERN |
    T_PRINTSEEDS T_PATTERN |
    T_PRINTUSAGE T_PATTERN |
    T_RENAMESOURCEFILEATTRIBUTE T_PATTERN |
    T_REPACKAGECLASSES T_PATTERN |
    T_USEUNIQUECLASSMEMBERNAMES |
    T_WHYAREYOUKEEPING T_PATTERN;

OPTIMIZATION_LIST:
    OPTIMIZATION_TERM T_COMMA OPTIMIZATION_LIST |
    OPTIMIZATION_TERM;

OPTIMIZATION_TERM:
    T_PATTERN |
    T_NOT T_PATTERN;

PATTERN_LIST:
    T_PATTERN T_COMMA PATTERN_LIST |
    T_PATTERN;

%%

bool parse_proguard_file(const char * file, std::vector<KeepRule>* passed_rules) {
    FILE *pgfile = fopen(file, "r");
    if (!pgfile) {
        std::cerr << "Couldn't open " << file << std::endl;
        return false;
    }
    yyin = pgfile;
    // parse through the input until there is no more:
    do {
        yyparse();
    } while (!feof(yyin));

    passed_rules->swap(*rules);

    return true;
}

void yyerror(char const * msg) {
    printf("Parse error on line %d: %s\n", line_number, msg);
}
