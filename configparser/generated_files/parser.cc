/* A Bison parser, made by GNU Bison 3.0.4.  */

/* Bison implementation for Yacc-like parsers in C

   Copyright (C) 1984, 1989-1990, 2000-2015 Free Software Foundation, Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

/* As a special exception, you may create a larger work that contains
   part or all of the Bison parser skeleton and distribute that work
   under terms of your choice, so long as that work isn't itself a
   parser generator using the skeleton or a modified version thereof
   as a parser skeleton.  Alternatively, if you modify or redistribute
   the parser skeleton itself, you may (at your option) remove this
   special exception, which will cause the skeleton and the resulting
   Bison output files to be licensed under the GNU General Public
   License without this special exception.

   This special exception was added by the Free Software Foundation in
   version 2.2 of Bison.  */

/* C LALR(1) parser skeleton written by Richard Stallman, by
   simplifying the original so-called "semantic" parser.  */

/* All symbols defined below should begin with yy or YY, to avoid
   infringing on user name space.  This should be done even for local
   variables, as they might otherwise be expanded by user macros.
   There are some unavoidable exceptions within include files to
   define necessary library symbols; they are noted "INFRINGES ON
   USER NAME SPACE" below.  */

/* Identify Bison output.  */
#define YYBISON 1

/* Bison version.  */
#define YYBISON_VERSION "3.0.4"

/* Skeleton name.  */
#define YYSKELETON_NAME "yacc.c"

/* Pure parsers.  */
#define YYPURE 0

/* Push parsers.  */
#define YYPUSH 0

/* Pull parsers.  */
#define YYPULL 1




/* Copy the first part of user declarations.  */
#line 1 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:339  */

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

/* TODO: consider conversion to using %parse-param to specify args for yyparse() */
std::vector<KeepRule>* rules = nullptr;
std::vector<std::string>* library_jars = nullptr;

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


#line 167 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:339  */

# ifndef YY_NULLPTR
#  if defined __cplusplus && 201103L <= __cplusplus
#   define YY_NULLPTR nullptr
#  else
#   define YY_NULLPTR 0
#  endif
# endif

/* Enabling verbose error messages.  */
#ifdef YYERROR_VERBOSE
# undef YYERROR_VERBOSE
# define YYERROR_VERBOSE 1
#else
# define YYERROR_VERBOSE 1
#endif

/* In a future release of Bison, this section will be replaced
   by #include "parser.hh".  */
#ifndef YY_YY_USERS_DRUSSI_FBSOURCE_FBANDROID_BUCK_OUT_GEN_NATIVE_REDEX_CONFIG_GENERATE_PARSER_CC_PARSER_HH_INCLUDED
# define YY_YY_USERS_DRUSSI_FBSOURCE_FBANDROID_BUCK_OUT_GEN_NATIVE_REDEX_CONFIG_GENERATE_PARSER_CC_PARSER_HH_INCLUDED
/* Debug traces.  */
#ifndef YYDEBUG
# define YYDEBUG 0
#endif
#if YYDEBUG
extern int yydebug;
#endif

/* Token type.  */
#ifndef YYTOKENTYPE
# define YYTOKENTYPE
  enum yytokentype
  {
    T_NEWLINE = 258,
    T_SEMICOLON = 259,
    T_COMMA = 260,
    T_NOT = 261,
    T_AT = 262,
    T_COMMENT = 263,
    T_KEEP = 264,
    T_KEEPNAMES = 265,
    T_KEEPCLASSMEMBERS = 266,
    T_KEEPCLASSMEMBERNAMES = 267,
    T_KEEPCLASSESWITHMEMBERS = 268,
    T_KEEPCLASSESWITHMEMBERNAMES = 269,
    T_ALLOWOBFUSCATION = 270,
    T_ALLOWOPTIMIZATION = 271,
    T_ALLOWSHRINKING = 272,
    T_ADAPTCLASSSTRINGS = 273,
    T_ADAPTRESOURCEFILECONTENTS = 274,
    T_ADAPTRESOURCEFILENAMES = 275,
    T_ALLOWACCESSMODIFICATION = 276,
    T_APPLYMAPPING = 277,
    T_ASSUMENOSIDEEFFECTS = 278,
    T_CLASSOBFUSCATIONDICTIONARY = 279,
    T_DONTOBFUSCATE = 280,
    T_DONTOPTIMIZE = 281,
    T_DONTPREVERIFY = 282,
    T_DONTSHRINK = 283,
    T_DONTWARN = 284,
    T_DONTUSEMIXEDCASECLASSNAMES = 285,
    T_DONTSKIPNONPUBLICLIBRARYCLASSES = 286,
    T_FLATTENPACKAGEHIERARCHY = 287,
    T_INJARS = 288,
    T_KEEPATTRIBUTES = 289,
    T_KEEPPACKAGENAMES = 290,
    T_KEEPPARAMETERNAMES = 291,
    T_LIBRARYJARS = 292,
    T_MERGEINTERFACESAGGRESSIVELY = 293,
    T_OBFUSCATIONDICTIONARY = 294,
    T_OPTIMIZATIONPASSES = 295,
    T_OPTIMIZATIONS = 296,
    T_OUTJARS = 297,
    T_OVERLOADAGGRESSIVELY = 298,
    T_PACKAGEOBFUSCATIONDICTIONARY = 299,
    T_PRINTCONFIGURATION = 300,
    T_PRINTMAPPING = 301,
    T_PRINTSEEDS = 302,
    T_PRINTUSAGE = 303,
    T_RENAMESOURCEFILEATTRIBUTE = 304,
    T_REPACKAGECLASSES = 305,
    T_USEUNIQUECLASSMEMBERNAMES = 306,
    T_VERBOSE = 307,
    T_WHYAREYOUKEEPING = 308,
    T_CLASS = 309,
    T_ENUM = 310,
    T_INTERFACE = 311,
    T_AT_INTERFACE = 312,
    T_INIT = 313,
    T_IMPLEMENTS = 314,
    T_EXTENDS = 315,
    T_PUBLIC = 316,
    T_PRIVATE = 317,
    T_PROTECTED = 318,
    T_STATIC = 319,
    T_FINAL = 320,
    T_TRANSIENT = 321,
    T_NATIVE = 322,
    T_METHODS = 323,
    T_FIELDS = 324,
    T_ANY_MEMBER = 325,
    T_PATTERN = 326,
    T_MEMBERS_BEGIN = 327,
    T_MEMBERS_END = 328,
    T_ARGS_BEGIN = 329,
    T_ARGS_END = 330
  };
#endif

/* Value type.  */
#if ! defined YYSTYPE && ! defined YYSTYPE_IS_DECLARED
typedef int YYSTYPE;
# define YYSTYPE_IS_TRIVIAL 1
# define YYSTYPE_IS_DECLARED 1
#endif


extern YYSTYPE yylval;

int yyparse (void);

#endif /* !YY_YY_USERS_DRUSSI_FBSOURCE_FBANDROID_BUCK_OUT_GEN_NATIVE_REDEX_CONFIG_GENERATE_PARSER_CC_PARSER_HH_INCLUDED  */

/* Copy the second part of user declarations.  */

#line 294 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:358  */

#ifdef short
# undef short
#endif

#ifdef YYTYPE_UINT8
typedef YYTYPE_UINT8 yytype_uint8;
#else
typedef unsigned char yytype_uint8;
#endif

#ifdef YYTYPE_INT8
typedef YYTYPE_INT8 yytype_int8;
#else
typedef signed char yytype_int8;
#endif

#ifdef YYTYPE_UINT16
typedef YYTYPE_UINT16 yytype_uint16;
#else
typedef unsigned short int yytype_uint16;
#endif

#ifdef YYTYPE_INT16
typedef YYTYPE_INT16 yytype_int16;
#else
typedef short int yytype_int16;
#endif

#ifndef YYSIZE_T
# ifdef __SIZE_TYPE__
#  define YYSIZE_T __SIZE_TYPE__
# elif defined size_t
#  define YYSIZE_T size_t
# elif ! defined YYSIZE_T
#  include <stddef.h> /* INFRINGES ON USER NAME SPACE */
#  define YYSIZE_T size_t
# else
#  define YYSIZE_T unsigned int
# endif
#endif

#define YYSIZE_MAXIMUM ((YYSIZE_T) -1)

#ifndef YY_
# if defined YYENABLE_NLS && YYENABLE_NLS
#  if ENABLE_NLS
#   include <libintl.h> /* INFRINGES ON USER NAME SPACE */
#   define YY_(Msgid) dgettext ("bison-runtime", Msgid)
#  endif
# endif
# ifndef YY_
#  define YY_(Msgid) Msgid
# endif
#endif

#ifndef YY_ATTRIBUTE
# if (defined __GNUC__                                               \
      && (2 < __GNUC__ || (__GNUC__ == 2 && 96 <= __GNUC_MINOR__)))  \
     || defined __SUNPRO_C && 0x5110 <= __SUNPRO_C
#  define YY_ATTRIBUTE(Spec) __attribute__(Spec)
# else
#  define YY_ATTRIBUTE(Spec) /* empty */
# endif
#endif

#ifndef YY_ATTRIBUTE_PURE
# define YY_ATTRIBUTE_PURE   YY_ATTRIBUTE ((__pure__))
#endif

#ifndef YY_ATTRIBUTE_UNUSED
# define YY_ATTRIBUTE_UNUSED YY_ATTRIBUTE ((__unused__))
#endif

#if !defined _Noreturn \
     && (!defined __STDC_VERSION__ || __STDC_VERSION__ < 201112)
# if defined _MSC_VER && 1200 <= _MSC_VER
#  define _Noreturn __declspec (noreturn)
# else
#  define _Noreturn YY_ATTRIBUTE ((__noreturn__))
# endif
#endif

/* Suppress unused-variable warnings by "using" E.  */
#if ! defined lint || defined __GNUC__
# define YYUSE(E) ((void) (E))
#else
# define YYUSE(E) /* empty */
#endif

#if defined __GNUC__ && 407 <= __GNUC__ * 100 + __GNUC_MINOR__
/* Suppress an incorrect diagnostic about yylval being uninitialized.  */
# define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN \
    _Pragma ("GCC diagnostic push") \
    _Pragma ("GCC diagnostic ignored \"-Wuninitialized\"")\
    _Pragma ("GCC diagnostic ignored \"-Wmaybe-uninitialized\"")
# define YY_IGNORE_MAYBE_UNINITIALIZED_END \
    _Pragma ("GCC diagnostic pop")
#else
# define YY_INITIAL_VALUE(Value) Value
#endif
#ifndef YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
# define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
# define YY_IGNORE_MAYBE_UNINITIALIZED_END
#endif
#ifndef YY_INITIAL_VALUE
# define YY_INITIAL_VALUE(Value) /* Nothing. */
#endif


#if ! defined yyoverflow || YYERROR_VERBOSE

/* The parser invokes alloca or malloc; define the necessary symbols.  */

# ifdef YYSTACK_USE_ALLOCA
#  if YYSTACK_USE_ALLOCA
#   ifdef __GNUC__
#    define YYSTACK_ALLOC __builtin_alloca
#   elif defined __BUILTIN_VA_ARG_INCR
#    include <alloca.h> /* INFRINGES ON USER NAME SPACE */
#   elif defined _AIX
#    define YYSTACK_ALLOC __alloca
#   elif defined _MSC_VER
#    include <malloc.h> /* INFRINGES ON USER NAME SPACE */
#    define alloca _alloca
#   else
#    define YYSTACK_ALLOC alloca
#    if ! defined _ALLOCA_H && ! defined EXIT_SUCCESS
#     include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
      /* Use EXIT_SUCCESS as a witness for stdlib.h.  */
#     ifndef EXIT_SUCCESS
#      define EXIT_SUCCESS 0
#     endif
#    endif
#   endif
#  endif
# endif

# ifdef YYSTACK_ALLOC
   /* Pacify GCC's 'empty if-body' warning.  */
#  define YYSTACK_FREE(Ptr) do { /* empty */; } while (0)
#  ifndef YYSTACK_ALLOC_MAXIMUM
    /* The OS might guarantee only one guard page at the bottom of the stack,
       and a page size can be as small as 4096 bytes.  So we cannot safely
       invoke alloca (N) if N exceeds 4096.  Use a slightly smaller number
       to allow for a few compiler-allocated temporary stack slots.  */
#   define YYSTACK_ALLOC_MAXIMUM 4032 /* reasonable circa 2006 */
#  endif
# else
#  define YYSTACK_ALLOC YYMALLOC
#  define YYSTACK_FREE YYFREE
#  ifndef YYSTACK_ALLOC_MAXIMUM
#   define YYSTACK_ALLOC_MAXIMUM YYSIZE_MAXIMUM
#  endif
#  if (defined __cplusplus && ! defined EXIT_SUCCESS \
       && ! ((defined YYMALLOC || defined malloc) \
             && (defined YYFREE || defined free)))
#   include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
#   ifndef EXIT_SUCCESS
#    define EXIT_SUCCESS 0
#   endif
#  endif
#  ifndef YYMALLOC
#   define YYMALLOC malloc
#   if ! defined malloc && ! defined EXIT_SUCCESS
void *malloc (YYSIZE_T); /* INFRINGES ON USER NAME SPACE */
#   endif
#  endif
#  ifndef YYFREE
#   define YYFREE free
#   if ! defined free && ! defined EXIT_SUCCESS
void free (void *); /* INFRINGES ON USER NAME SPACE */
#   endif
#  endif
# endif
#endif /* ! defined yyoverflow || YYERROR_VERBOSE */


#if (! defined yyoverflow \
     && (! defined __cplusplus \
         || (defined YYSTYPE_IS_TRIVIAL && YYSTYPE_IS_TRIVIAL)))

/* A type that is properly aligned for any stack member.  */
union yyalloc
{
  yytype_int16 yyss_alloc;
  YYSTYPE yyvs_alloc;
};

/* The size of the maximum gap between one aligned stack and the next.  */
# define YYSTACK_GAP_MAXIMUM (sizeof (union yyalloc) - 1)

/* The size of an array large to enough to hold all stacks, each with
   N elements.  */
# define YYSTACK_BYTES(N) \
     ((N) * (sizeof (yytype_int16) + sizeof (YYSTYPE)) \
      + YYSTACK_GAP_MAXIMUM)

# define YYCOPY_NEEDED 1

/* Relocate STACK from its old location to the new one.  The
   local variables YYSIZE and YYSTACKSIZE give the old and new number of
   elements in the stack, and YYPTR gives the new location of the
   stack.  Advance YYPTR to a properly aligned location for the next
   stack.  */
# define YYSTACK_RELOCATE(Stack_alloc, Stack)                           \
    do                                                                  \
      {                                                                 \
        YYSIZE_T yynewbytes;                                            \
        YYCOPY (&yyptr->Stack_alloc, Stack, yysize);                    \
        Stack = &yyptr->Stack_alloc;                                    \
        yynewbytes = yystacksize * sizeof (*Stack) + YYSTACK_GAP_MAXIMUM; \
        yyptr += yynewbytes / sizeof (*yyptr);                          \
      }                                                                 \
    while (0)

#endif

#if defined YYCOPY_NEEDED && YYCOPY_NEEDED
/* Copy COUNT objects from SRC to DST.  The source and destination do
   not overlap.  */
# ifndef YYCOPY
#  if defined __GNUC__ && 1 < __GNUC__
#   define YYCOPY(Dst, Src, Count) \
      __builtin_memcpy (Dst, Src, (Count) * sizeof (*(Src)))
#  else
#   define YYCOPY(Dst, Src, Count)              \
      do                                        \
        {                                       \
          YYSIZE_T yyi;                         \
          for (yyi = 0; yyi < (Count); yyi++)   \
            (Dst)[yyi] = (Src)[yyi];            \
        }                                       \
      while (0)
#  endif
# endif
#endif /* !YYCOPY_NEEDED */

/* YYFINAL -- State number of the termination state.  */
#define YYFINAL  74
/* YYLAST -- Last index in YYTABLE.  */
#define YYLAST   194

/* YYNTOKENS -- Number of terminals.  */
#define YYNTOKENS  76
/* YYNNTS -- Number of nonterminals.  */
#define YYNNTS  39
/* YYNRULES -- Number of rules.  */
#define YYNRULES  115
/* YYNSTATES -- Number of states.  */
#define YYNSTATES  163

/* YYTRANSLATE[YYX] -- Symbol number corresponding to YYX as returned
   by yylex, with out-of-bounds checking.  */
#define YYUNDEFTOK  2
#define YYMAXUTOK   330

#define YYTRANSLATE(YYX)                                                \
  ((unsigned int) (YYX) <= YYMAXUTOK ? yytranslate[YYX] : YYUNDEFTOK)

/* YYTRANSLATE[TOKEN-NUM] -- Symbol number corresponding to TOKEN-NUM
   as returned by yylex, without out-of-bounds checking.  */
static const yytype_uint8 yytranslate[] =
{
       0,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     1,     2,     3,     4,
       5,     6,     7,     8,     9,    10,    11,    12,    13,    14,
      15,    16,    17,    18,    19,    20,    21,    22,    23,    24,
      25,    26,    27,    28,    29,    30,    31,    32,    33,    34,
      35,    36,    37,    38,    39,    40,    41,    42,    43,    44,
      45,    46,    47,    48,    49,    50,    51,    52,    53,    54,
      55,    56,    57,    58,    59,    60,    61,    62,    63,    64,
      65,    66,    67,    68,    69,    70,    71,    72,    73,    74,
      75
};

#if YYDEBUG
  /* YYRLINE[YYN] -- Source line where rule number YYN was defined.  */
static const yytype_uint16 yyrline[] =
{
       0,   178,   178,   178,   183,   184,   187,   188,   189,   190,
     193,   194,   195,   199,   199,   204,   206,   208,   210,   212,
     214,   217,   219,   222,   223,   224,   227,   227,   230,   233,
     234,   235,   236,   237,   240,   243,   244,   245,   247,   249,
     252,   253,   256,   256,   264,   266,   268,   270,   271,   272,
     274,   276,   279,   280,   283,   284,   285,   286,   289,   290,
     291,   292,   293,   293,   294,   294,   295,   295,   298,   300,
     300,   301,   301,   301,   304,   305,   308,   311,   312,   313,
     314,   315,   316,   317,   318,   319,   320,   321,   322,   323,
     324,   325,   326,   327,   328,   329,   330,   331,   332,   333,
     334,   335,   336,   337,   338,   339,   340,   341,   342,   343,
     346,   347,   350,   351,   354,   355
};
#endif

#if YYDEBUG || YYERROR_VERBOSE || 1
/* YYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
   First, the terminals, then, starting at YYNTOKENS, nonterminals.  */
static const char *const yytname[] =
{
  "$end", "error", "$undefined", "T_NEWLINE", "T_SEMICOLON", "T_COMMA",
  "T_NOT", "T_AT", "T_COMMENT", "T_KEEP", "T_KEEPNAMES",
  "T_KEEPCLASSMEMBERS", "T_KEEPCLASSMEMBERNAMES",
  "T_KEEPCLASSESWITHMEMBERS", "T_KEEPCLASSESWITHMEMBERNAMES",
  "T_ALLOWOBFUSCATION", "T_ALLOWOPTIMIZATION", "T_ALLOWSHRINKING",
  "T_ADAPTCLASSSTRINGS", "T_ADAPTRESOURCEFILECONTENTS",
  "T_ADAPTRESOURCEFILENAMES", "T_ALLOWACCESSMODIFICATION",
  "T_APPLYMAPPING", "T_ASSUMENOSIDEEFFECTS",
  "T_CLASSOBFUSCATIONDICTIONARY", "T_DONTOBFUSCATE", "T_DONTOPTIMIZE",
  "T_DONTPREVERIFY", "T_DONTSHRINK", "T_DONTWARN",
  "T_DONTUSEMIXEDCASECLASSNAMES", "T_DONTSKIPNONPUBLICLIBRARYCLASSES",
  "T_FLATTENPACKAGEHIERARCHY", "T_INJARS", "T_KEEPATTRIBUTES",
  "T_KEEPPACKAGENAMES", "T_KEEPPARAMETERNAMES", "T_LIBRARYJARS",
  "T_MERGEINTERFACESAGGRESSIVELY", "T_OBFUSCATIONDICTIONARY",
  "T_OPTIMIZATIONPASSES", "T_OPTIMIZATIONS", "T_OUTJARS",
  "T_OVERLOADAGGRESSIVELY", "T_PACKAGEOBFUSCATIONDICTIONARY",
  "T_PRINTCONFIGURATION", "T_PRINTMAPPING", "T_PRINTSEEDS", "T_PRINTUSAGE",
  "T_RENAMESOURCEFILEATTRIBUTE", "T_REPACKAGECLASSES",
  "T_USEUNIQUECLASSMEMBERNAMES", "T_VERBOSE", "T_WHYAREYOUKEEPING",
  "T_CLASS", "T_ENUM", "T_INTERFACE", "T_AT_INTERFACE", "T_INIT",
  "T_IMPLEMENTS", "T_EXTENDS", "T_PUBLIC", "T_PRIVATE", "T_PROTECTED",
  "T_STATIC", "T_FINAL", "T_TRANSIENT", "T_NATIVE", "T_METHODS",
  "T_FIELDS", "T_ANY_MEMBER", "T_PATTERN", "T_MEMBERS_BEGIN",
  "T_MEMBERS_END", "T_ARGS_BEGIN", "T_ARGS_END", "$accept", "START",
  "RULE_LIST", "RULE", "DIRECTIVE", "KEEP_RULE", "$@1", "KEEP_TYPE",
  "KEEP_MODIFIERS", "ALLOWED_OPERATION", "CLASS_FILTER", "$@2",
  "CLASS_SPEC", "CLASS_TYPE", "CLASS_NAME", "IMPLEMENTS_OR_EXTENDS",
  "CLASS_MEMBERS", "MEMBERS_LIST", "MEMBER", "$@3", "ANNOTATION",
  "VISIBILITY", "ATTRIBUTES", "ATTRIBUTE_TERM", "ATTRIBUTE", "MEMBER_NAME",
  "$@4", "$@5", "$@6", "ARGS", "$@7", "$@8", "$@9", "ARGS_LIST", "ARG",
  "UNSUPPORTED_PROGUARD_RULE", "OPTIMIZATION_LIST", "OPTIMIZATION_TERM",
  "PATTERN_LIST", YY_NULLPTR
};
#endif

# ifdef YYPRINT
/* YYTOKNUM[NUM] -- (External) token number corresponding to the
   (internal) symbol number NUM (which must be that of a token).  */
static const yytype_uint16 yytoknum[] =
{
       0,   256,   257,   258,   259,   260,   261,   262,   263,   264,
     265,   266,   267,   268,   269,   270,   271,   272,   273,   274,
     275,   276,   277,   278,   279,   280,   281,   282,   283,   284,
     285,   286,   287,   288,   289,   290,   291,   292,   293,   294,
     295,   296,   297,   298,   299,   300,   301,   302,   303,   304,
     305,   306,   307,   308,   309,   310,   311,   312,   313,   314,
     315,   316,   317,   318,   319,   320,   321,   322,   323,   324,
     325,   326,   327,   328,   329,   330
};
# endif

#define YYPACT_NINF -110

#define yypact_value_is_default(Yystate) \
  (!!((Yystate) == (-110)))

#define YYTABLE_NINF -72

#define yytable_value_is_error(Yytable_value) \
  0

  /* YYPACT[STATE-NUM] -- Index in YYTABLE of the portion describing
     STATE-NUM.  */
static const yytype_int16 yypact[] =
{
       4,  -110,   -65,   -64,   -57,  -110,   -12,  -110,   -11,  -110,
    -110,  -110,  -110,    -6,  -110,  -110,    52,    53,    -6,    -6,
    -110,    54,  -110,    55,    56,    -4,    57,  -110,    59,    -6,
      60,    63,    64,    65,    66,  -110,  -110,    67,    68,    58,
    -110,  -110,  -110,    61,  -110,  -110,  -110,  -110,  -110,    69,
     122,  -110,   127,  -110,  -110,  -110,  -110,  -110,  -110,  -110,
    -110,    71,  -110,  -110,   128,  -110,  -110,  -110,  -110,  -110,
    -110,  -110,  -110,  -110,  -110,  -110,  -110,  -110,  -110,  -110,
    -110,  -110,   134,  -110,  -110,    72,   -42,    -6,  -110,    -4,
     105,  -110,    73,   136,   122,  -110,  -110,  -110,  -110,    -3,
    -110,  -110,  -110,  -110,  -110,   134,    69,  -110,   140,  -110,
     -42,    48,  -110,  -110,  -110,  -110,    62,    -3,  -110,  -110,
    -110,  -110,    -3,  -110,  -110,  -110,  -110,  -110,   -50,    74,
    -110,   -53,    76,    77,  -110,  -110,  -110,  -110,  -110,  -110,
      78,    79,    80,  -110,  -110,    81,    83,    84,    85,  -110,
    -110,  -110,  -110,    82,    87,  -110,  -110,   146,  -110,    87,
      86,  -110,  -110
};

  /* YYDEFACT[STATE-NUM] -- Default reduction number in state STATE-NUM.
     Performed when YYTABLE does not specify something else to do.  Zero
     means the default is an error.  */
static const yytype_uint8 yydefact[] =
{
      13,     6,     0,     0,     0,    80,     0,    26,     0,    84,
      85,    86,    87,     0,    89,    90,     0,     0,     0,     0,
      94,     0,    95,     0,     0,     0,     0,    99,     0,     0,
       0,     0,     0,     0,     0,   107,   108,     0,     0,    13,
       5,     9,     8,     0,     7,    77,    78,    79,    81,    38,
      44,    83,   115,    88,    91,    10,    92,    93,    12,    96,
      97,     0,   112,    98,   111,    11,   100,   101,   102,   103,
     104,   105,   106,   109,     1,     4,    15,    16,    17,    18,
      19,    20,    21,    42,    82,     0,    46,     0,   113,     0,
       0,    26,    42,     0,    44,    45,    47,    48,    49,    50,
     114,   110,    23,    24,    25,    21,    38,    39,     0,    41,
      46,     0,    54,    55,    56,    57,    29,    50,    53,    22,
      14,    40,    50,    52,    30,    31,    32,    33,    35,     0,
      51,     0,     0,     0,    27,    34,    28,    58,    60,    59,
      61,    64,    68,    36,    37,     0,     0,     0,    69,    43,
      63,    65,    67,     0,     0,    70,    76,    72,    75,     0,
       0,    74,    73
};

  /* YYPGOTO[NTERM-NUM].  */
static const yytype_int8 yypgoto[] =
{
    -110,  -110,  -110,   120,  -110,  -110,  -110,  -110,    70,  -110,
      75,  -110,  -110,  -110,  -110,  -110,    88,  -110,    89,  -110,
      90,    50,  -109,  -110,    51,  -110,  -110,  -110,  -110,  -110,
    -110,  -110,  -110,  -110,     5,  -110,    91,  -110,   -18
};

  /* YYDEFGOTO[NTERM-NUM].  */
static const yytype_int16 yydefgoto[] =
{
      -1,    38,    39,    40,    41,    42,    43,    82,    91,   105,
      49,    50,   128,   129,   136,   134,    84,    92,    93,    94,
      86,    99,   116,   117,   118,   142,   145,   146,   147,   149,
     153,   154,   160,   157,   158,    44,    63,    64,    53
};

  /* YYTABLE[YYPACT[STATE-NUM]] -- What to do in state STATE-NUM.  If
     positive, shift that token.  If negative, reduce the rule whose
     number is the opposite.  If YYTABLE_NINF, syntax error.  */
static const yytype_int16 yytable[] =
{
      56,    57,    61,   111,    -3,   137,    45,    46,   130,   132,
     133,    67,     1,   131,    47,   138,   139,   140,   141,    96,
      97,    98,     2,     3,     4,     5,     6,     7,     8,     9,
      10,    11,    12,    13,    14,    15,    16,    17,    18,    19,
      20,    21,    22,    23,    24,    25,    26,    27,    28,    29,
      30,    31,    32,    33,    34,    35,    36,    37,    -2,    48,
      51,   112,   113,   114,   115,    52,     1,    62,    74,   100,
      76,    77,    78,    79,    80,    81,     2,     3,     4,     5,
       6,     7,     8,     9,    10,    11,    12,    13,    14,    15,
      16,    17,    18,    19,    20,    21,    22,    23,    24,    25,
      26,    27,    28,    29,    30,    31,    32,    33,    34,    35,
      36,    37,   112,   113,   114,   115,   124,   125,   126,   127,
     102,   103,   104,    54,    55,    58,    59,    60,    65,    85,
      66,    68,    87,    89,    69,    70,    71,    72,    73,    90,
     109,    83,    88,    95,   121,   135,   107,   143,   144,   -62,
     -66,   159,   150,   151,   148,   152,   -71,   155,   156,    75,
     122,   162,   123,     0,   161,     0,   106,     0,     0,     0,
       0,     0,     0,     0,     0,   119,     0,     0,     0,     0,
     101,   108,     0,     0,   110,     0,     0,     0,     0,     0,
       0,     0,     0,     0,   120
};

static const yytype_int16 yycheck[] =
{
      18,    19,     6,     6,     0,    58,    71,    71,   117,    59,
      60,    29,     8,   122,    71,    68,    69,    70,    71,    61,
      62,    63,    18,    19,    20,    21,    22,    23,    24,    25,
      26,    27,    28,    29,    30,    31,    32,    33,    34,    35,
      36,    37,    38,    39,    40,    41,    42,    43,    44,    45,
      46,    47,    48,    49,    50,    51,    52,    53,     0,    71,
      71,    64,    65,    66,    67,    71,     8,    71,     0,    87,
       9,    10,    11,    12,    13,    14,    18,    19,    20,    21,
      22,    23,    24,    25,    26,    27,    28,    29,    30,    31,
      32,    33,    34,    35,    36,    37,    38,    39,    40,    41,
      42,    43,    44,    45,    46,    47,    48,    49,    50,    51,
      52,    53,    64,    65,    66,    67,    54,    55,    56,    57,
      15,    16,    17,    71,    71,    71,    71,    71,    71,     7,
      71,    71,     5,     5,    71,    71,    71,    71,    71,     5,
       4,    72,    71,    71,     4,    71,    73,    71,    71,    71,
      71,     5,    71,    70,    74,    71,    71,    75,    71,    39,
     110,    75,   111,    -1,   159,    -1,    91,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,   105,    -1,    -1,    -1,    -1,
      89,    92,    -1,    -1,    94,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,   106
};

  /* YYSTOS[STATE-NUM] -- The (internal number of the) accessing
     symbol of state STATE-NUM.  */
static const yytype_uint8 yystos[] =
{
       0,     8,    18,    19,    20,    21,    22,    23,    24,    25,
      26,    27,    28,    29,    30,    31,    32,    33,    34,    35,
      36,    37,    38,    39,    40,    41,    42,    43,    44,    45,
      46,    47,    48,    49,    50,    51,    52,    53,    77,    78,
      79,    80,    81,    82,   111,    71,    71,    71,    71,    86,
      87,    71,    71,   114,    71,    71,   114,   114,    71,    71,
      71,     6,    71,   112,   113,    71,    71,   114,    71,    71,
      71,    71,    71,    71,     0,    79,     9,    10,    11,    12,
      13,    14,    83,    72,    92,     7,    96,     5,    71,     5,
       5,    84,    93,    94,    95,    71,    61,    62,    63,    97,
     114,   112,    15,    16,    17,    85,    86,    73,    94,     4,
      96,     6,    64,    65,    66,    67,    98,    99,   100,    84,
      92,     4,    97,   100,    54,    55,    56,    57,    88,    89,
      98,    98,    59,    60,    91,    71,    90,    58,    68,    69,
      70,    71,   101,    71,    71,   102,   103,   104,    74,   105,
      71,    70,    71,   106,   107,    75,    71,   109,   110,     5,
     108,   110,    75
};

  /* YYR1[YYN] -- Symbol number of symbol that rule YYN derives.  */
static const yytype_uint8 yyr1[] =
{
       0,    76,    77,    77,    78,    78,    79,    79,    79,    79,
      80,    80,    80,    82,    81,    83,    83,    83,    83,    83,
      83,    84,    84,    85,    85,    85,    87,    86,    88,    89,
      89,    89,    89,    89,    90,    91,    91,    91,    92,    92,
      93,    93,    95,    94,    96,    96,    97,    97,    97,    97,
      98,    98,    99,    99,   100,   100,   100,   100,   101,   101,
     101,   101,   102,   101,   103,   101,   104,   101,   105,   106,
     105,   107,   108,   105,   109,   109,   110,   111,   111,   111,
     111,   111,   111,   111,   111,   111,   111,   111,   111,   111,
     111,   111,   111,   111,   111,   111,   111,   111,   111,   111,
     111,   111,   111,   111,   111,   111,   111,   111,   111,   111,
     112,   112,   113,   113,   114,   114
};

  /* YYR2[YYN] -- Number of symbols on the right hand side of rule YYN.  */
static const yytype_uint8 yyr2[] =
{
       0,     2,     1,     0,     2,     1,     1,     1,     1,     1,
       2,     2,     2,     0,     5,     1,     1,     1,     1,     1,
       1,     0,     3,     1,     1,     1,     0,     6,     2,     0,
       1,     1,     1,     1,     1,     0,     2,     2,     0,     3,
       3,     2,     0,     6,     0,     2,     0,     1,     1,     1,
       0,     2,     2,     1,     1,     1,     1,     1,     1,     1,
       1,     1,     0,     3,     0,     3,     0,     3,     0,     0,
       3,     0,     0,     5,     3,     1,     1,     2,     2,     2,
       1,     2,     3,     2,     1,     1,     1,     1,     2,     1,
       1,     2,     2,     2,     1,     1,     2,     2,     2,     1,
       2,     2,     2,     2,     2,     2,     2,     1,     1,     2,
       3,     1,     1,     2,     3,     1
};


#define yyerrok         (yyerrstatus = 0)
#define yyclearin       (yychar = YYEMPTY)
#define YYEMPTY         (-2)
#define YYEOF           0

#define YYACCEPT        goto yyacceptlab
#define YYABORT         goto yyabortlab
#define YYERROR         goto yyerrorlab


#define YYRECOVERING()  (!!yyerrstatus)

#define YYBACKUP(Token, Value)                                  \
do                                                              \
  if (yychar == YYEMPTY)                                        \
    {                                                           \
      yychar = (Token);                                         \
      yylval = (Value);                                         \
      YYPOPSTACK (yylen);                                       \
      yystate = *yyssp;                                         \
      goto yybackup;                                            \
    }                                                           \
  else                                                          \
    {                                                           \
      yyerror (YY_("syntax error: cannot back up")); \
      YYERROR;                                                  \
    }                                                           \
while (0)

/* Error token number */
#define YYTERROR        1
#define YYERRCODE       256



/* Enable debugging if requested.  */
#if YYDEBUG

# ifndef YYFPRINTF
#  include <stdio.h> /* INFRINGES ON USER NAME SPACE */
#  define YYFPRINTF fprintf
# endif

# define YYDPRINTF(Args)                        \
do {                                            \
  if (yydebug)                                  \
    YYFPRINTF Args;                             \
} while (0)

/* This macro is provided for backward compatibility. */
#ifndef YY_LOCATION_PRINT
# define YY_LOCATION_PRINT(File, Loc) ((void) 0)
#endif


# define YY_SYMBOL_PRINT(Title, Type, Value, Location)                    \
do {                                                                      \
  if (yydebug)                                                            \
    {                                                                     \
      YYFPRINTF (stderr, "%s ", Title);                                   \
      yy_symbol_print (stderr,                                            \
                  Type, Value); \
      YYFPRINTF (stderr, "\n");                                           \
    }                                                                     \
} while (0)


/*----------------------------------------.
| Print this symbol's value on YYOUTPUT.  |
`----------------------------------------*/

static void
yy_symbol_value_print (FILE *yyoutput, int yytype, YYSTYPE const * const yyvaluep)
{
  FILE *yyo = yyoutput;
  YYUSE (yyo);
  if (!yyvaluep)
    return;
# ifdef YYPRINT
  if (yytype < YYNTOKENS)
    YYPRINT (yyoutput, yytoknum[yytype], *yyvaluep);
# endif
  YYUSE (yytype);
}


/*--------------------------------.
| Print this symbol on YYOUTPUT.  |
`--------------------------------*/

static void
yy_symbol_print (FILE *yyoutput, int yytype, YYSTYPE const * const yyvaluep)
{
  YYFPRINTF (yyoutput, "%s %s (",
             yytype < YYNTOKENS ? "token" : "nterm", yytname[yytype]);

  yy_symbol_value_print (yyoutput, yytype, yyvaluep);
  YYFPRINTF (yyoutput, ")");
}

/*------------------------------------------------------------------.
| yy_stack_print -- Print the state stack from its BOTTOM up to its |
| TOP (included).                                                   |
`------------------------------------------------------------------*/

static void
yy_stack_print (yytype_int16 *yybottom, yytype_int16 *yytop)
{
  YYFPRINTF (stderr, "Stack now");
  for (; yybottom <= yytop; yybottom++)
    {
      int yybot = *yybottom;
      YYFPRINTF (stderr, " %d", yybot);
    }
  YYFPRINTF (stderr, "\n");
}

# define YY_STACK_PRINT(Bottom, Top)                            \
do {                                                            \
  if (yydebug)                                                  \
    yy_stack_print ((Bottom), (Top));                           \
} while (0)


/*------------------------------------------------.
| Report that the YYRULE is going to be reduced.  |
`------------------------------------------------*/

static void
yy_reduce_print (yytype_int16 *yyssp, YYSTYPE *yyvsp, int yyrule)
{
  unsigned long int yylno = yyrline[yyrule];
  int yynrhs = yyr2[yyrule];
  int yyi;
  YYFPRINTF (stderr, "Reducing stack by rule %d (line %lu):\n",
             yyrule - 1, yylno);
  /* The symbols being reduced.  */
  for (yyi = 0; yyi < yynrhs; yyi++)
    {
      YYFPRINTF (stderr, "   $%d = ", yyi + 1);
      yy_symbol_print (stderr,
                       yystos[yyssp[yyi + 1 - yynrhs]],
                       &(yyvsp[(yyi + 1) - (yynrhs)])
                                              );
      YYFPRINTF (stderr, "\n");
    }
}

# define YY_REDUCE_PRINT(Rule)          \
do {                                    \
  if (yydebug)                          \
    yy_reduce_print (yyssp, yyvsp, Rule); \
} while (0)

/* Nonzero means print parse trace.  It is left uninitialized so that
   multiple parsers can coexist.  */
int yydebug;
#else /* !YYDEBUG */
# define YYDPRINTF(Args)
# define YY_SYMBOL_PRINT(Title, Type, Value, Location)
# define YY_STACK_PRINT(Bottom, Top)
# define YY_REDUCE_PRINT(Rule)
#endif /* !YYDEBUG */


/* YYINITDEPTH -- initial size of the parser's stacks.  */
#ifndef YYINITDEPTH
# define YYINITDEPTH 200
#endif

/* YYMAXDEPTH -- maximum size the stacks can grow to (effective only
   if the built-in stack extension method is used).

   Do not make this value too large; the results are undefined if
   YYSTACK_ALLOC_MAXIMUM < YYSTACK_BYTES (YYMAXDEPTH)
   evaluated with infinite-precision integer arithmetic.  */

#ifndef YYMAXDEPTH
# define YYMAXDEPTH 10000
#endif


#if YYERROR_VERBOSE

# ifndef yystrlen
#  if defined __GLIBC__ && defined _STRING_H
#   define yystrlen strlen
#  else
/* Return the length of YYSTR.  */
static YYSIZE_T
yystrlen (const char *yystr)
{
  YYSIZE_T yylen;
  for (yylen = 0; yystr[yylen]; yylen++)
    continue;
  return yylen;
}
#  endif
# endif

# ifndef yystpcpy
#  if defined __GLIBC__ && defined _STRING_H && defined _GNU_SOURCE
#   define yystpcpy stpcpy
#  else
/* Copy YYSRC to YYDEST, returning the address of the terminating '\0' in
   YYDEST.  */
static char *
yystpcpy (char *yydest, const char *yysrc)
{
  char *yyd = yydest;
  const char *yys = yysrc;

  while ((*yyd++ = *yys++) != '\0')
    continue;

  return yyd - 1;
}
#  endif
# endif

# ifndef yytnamerr
/* Copy to YYRES the contents of YYSTR after stripping away unnecessary
   quotes and backslashes, so that it's suitable for yyerror.  The
   heuristic is that double-quoting is unnecessary unless the string
   contains an apostrophe, a comma, or backslash (other than
   backslash-backslash).  YYSTR is taken from yytname.  If YYRES is
   null, do not copy; instead, return the length of what the result
   would have been.  */
static YYSIZE_T
yytnamerr (char *yyres, const char *yystr)
{
  if (*yystr == '"')
    {
      YYSIZE_T yyn = 0;
      char const *yyp = yystr;

      for (;;)
        switch (*++yyp)
          {
          case '\'':
          case ',':
            goto do_not_strip_quotes;

          case '\\':
            if (*++yyp != '\\')
              goto do_not_strip_quotes;
            /* Fall through.  */
          default:
            if (yyres)
              yyres[yyn] = *yyp;
            yyn++;
            break;

          case '"':
            if (yyres)
              yyres[yyn] = '\0';
            return yyn;
          }
    do_not_strip_quotes: ;
    }

  if (! yyres)
    return yystrlen (yystr);

  return yystpcpy (yyres, yystr) - yyres;
}
# endif

/* Copy into *YYMSG, which is of size *YYMSG_ALLOC, an error message
   about the unexpected token YYTOKEN for the state stack whose top is
   YYSSP.

   Return 0 if *YYMSG was successfully written.  Return 1 if *YYMSG is
   not large enough to hold the message.  In that case, also set
   *YYMSG_ALLOC to the required number of bytes.  Return 2 if the
   required number of bytes is too large to store.  */
static int
yysyntax_error (YYSIZE_T *yymsg_alloc, char **yymsg,
                yytype_int16 *yyssp, int yytoken)
{
  YYSIZE_T yysize0 = yytnamerr (YY_NULLPTR, yytname[yytoken]);
  YYSIZE_T yysize = yysize0;
  enum { YYERROR_VERBOSE_ARGS_MAXIMUM = 5 };
  /* Internationalized format string. */
  const char *yyformat = YY_NULLPTR;
  /* Arguments of yyformat. */
  char const *yyarg[YYERROR_VERBOSE_ARGS_MAXIMUM];
  /* Number of reported tokens (one for the "unexpected", one per
     "expected"). */
  int yycount = 0;

  /* There are many possibilities here to consider:
     - If this state is a consistent state with a default action, then
       the only way this function was invoked is if the default action
       is an error action.  In that case, don't check for expected
       tokens because there are none.
     - The only way there can be no lookahead present (in yychar) is if
       this state is a consistent state with a default action.  Thus,
       detecting the absence of a lookahead is sufficient to determine
       that there is no unexpected or expected token to report.  In that
       case, just report a simple "syntax error".
     - Don't assume there isn't a lookahead just because this state is a
       consistent state with a default action.  There might have been a
       previous inconsistent state, consistent state with a non-default
       action, or user semantic action that manipulated yychar.
     - Of course, the expected token list depends on states to have
       correct lookahead information, and it depends on the parser not
       to perform extra reductions after fetching a lookahead from the
       scanner and before detecting a syntax error.  Thus, state merging
       (from LALR or IELR) and default reductions corrupt the expected
       token list.  However, the list is correct for canonical LR with
       one exception: it will still contain any token that will not be
       accepted due to an error action in a later state.
  */
  if (yytoken != YYEMPTY)
    {
      int yyn = yypact[*yyssp];
      yyarg[yycount++] = yytname[yytoken];
      if (!yypact_value_is_default (yyn))
        {
          /* Start YYX at -YYN if negative to avoid negative indexes in
             YYCHECK.  In other words, skip the first -YYN actions for
             this state because they are default actions.  */
          int yyxbegin = yyn < 0 ? -yyn : 0;
          /* Stay within bounds of both yycheck and yytname.  */
          int yychecklim = YYLAST - yyn + 1;
          int yyxend = yychecklim < YYNTOKENS ? yychecklim : YYNTOKENS;
          int yyx;

          for (yyx = yyxbegin; yyx < yyxend; ++yyx)
            if (yycheck[yyx + yyn] == yyx && yyx != YYTERROR
                && !yytable_value_is_error (yytable[yyx + yyn]))
              {
                if (yycount == YYERROR_VERBOSE_ARGS_MAXIMUM)
                  {
                    yycount = 1;
                    yysize = yysize0;
                    break;
                  }
                yyarg[yycount++] = yytname[yyx];
                {
                  YYSIZE_T yysize1 = yysize + yytnamerr (YY_NULLPTR, yytname[yyx]);
                  if (! (yysize <= yysize1
                         && yysize1 <= YYSTACK_ALLOC_MAXIMUM))
                    return 2;
                  yysize = yysize1;
                }
              }
        }
    }

  switch (yycount)
    {
# define YYCASE_(N, S)                      \
      case N:                               \
        yyformat = S;                       \
      break
      YYCASE_(0, YY_("syntax error"));
      YYCASE_(1, YY_("syntax error, unexpected %s"));
      YYCASE_(2, YY_("syntax error, unexpected %s, expecting %s"));
      YYCASE_(3, YY_("syntax error, unexpected %s, expecting %s or %s"));
      YYCASE_(4, YY_("syntax error, unexpected %s, expecting %s or %s or %s"));
      YYCASE_(5, YY_("syntax error, unexpected %s, expecting %s or %s or %s or %s"));
# undef YYCASE_
    }

  {
    YYSIZE_T yysize1 = yysize + yystrlen (yyformat);
    if (! (yysize <= yysize1 && yysize1 <= YYSTACK_ALLOC_MAXIMUM))
      return 2;
    yysize = yysize1;
  }

  if (*yymsg_alloc < yysize)
    {
      *yymsg_alloc = 2 * yysize;
      if (! (yysize <= *yymsg_alloc
             && *yymsg_alloc <= YYSTACK_ALLOC_MAXIMUM))
        *yymsg_alloc = YYSTACK_ALLOC_MAXIMUM;
      return 1;
    }

  /* Avoid sprintf, as that infringes on the user's name space.
     Don't have undefined behavior even if the translation
     produced a string with the wrong number of "%s"s.  */
  {
    char *yyp = *yymsg;
    int yyi = 0;
    while ((*yyp = *yyformat) != '\0')
      if (*yyp == '%' && yyformat[1] == 's' && yyi < yycount)
        {
          yyp += yytnamerr (yyp, yyarg[yyi++]);
          yyformat += 2;
        }
      else
        {
          yyp++;
          yyformat++;
        }
  }
  return 0;
}
#endif /* YYERROR_VERBOSE */

/*-----------------------------------------------.
| Release the memory associated to this symbol.  |
`-----------------------------------------------*/

static void
yydestruct (const char *yymsg, int yytype, YYSTYPE *yyvaluep)
{
  YYUSE (yyvaluep);
  if (!yymsg)
    yymsg = "Deleting";
  YY_SYMBOL_PRINT (yymsg, yytype, yyvaluep, yylocationp);

  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  YYUSE (yytype);
  YY_IGNORE_MAYBE_UNINITIALIZED_END
}




/* The lookahead symbol.  */
int yychar;

/* The semantic value of the lookahead symbol.  */
YYSTYPE yylval;
/* Number of syntax errors so far.  */
int yynerrs;


/*----------.
| yyparse.  |
`----------*/

int
yyparse (void)
{
    int yystate;
    /* Number of tokens to shift before error messages enabled.  */
    int yyerrstatus;

    /* The stacks and their tools:
       'yyss': related to states.
       'yyvs': related to semantic values.

       Refer to the stacks through separate pointers, to allow yyoverflow
       to reallocate them elsewhere.  */

    /* The state stack.  */
    yytype_int16 yyssa[YYINITDEPTH];
    yytype_int16 *yyss;
    yytype_int16 *yyssp;

    /* The semantic value stack.  */
    YYSTYPE yyvsa[YYINITDEPTH];
    YYSTYPE *yyvs;
    YYSTYPE *yyvsp;

    YYSIZE_T yystacksize;

  int yyn;
  int yyresult;
  /* Lookahead token as an internal (translated) token number.  */
  int yytoken = 0;
  /* The variables used to return semantic value and location from the
     action routines.  */
  YYSTYPE yyval;

#if YYERROR_VERBOSE
  /* Buffer for error messages, and its allocated size.  */
  char yymsgbuf[128];
  char *yymsg = yymsgbuf;
  YYSIZE_T yymsg_alloc = sizeof yymsgbuf;
#endif

#define YYPOPSTACK(N)   (yyvsp -= (N), yyssp -= (N))

  /* The number of symbols on the RHS of the reduced rule.
     Keep to zero when no symbol should be popped.  */
  int yylen = 0;

  yyssp = yyss = yyssa;
  yyvsp = yyvs = yyvsa;
  yystacksize = YYINITDEPTH;

  YYDPRINTF ((stderr, "Starting parse\n"));

  yystate = 0;
  yyerrstatus = 0;
  yynerrs = 0;
  yychar = YYEMPTY; /* Cause a token to be read.  */
  goto yysetstate;

/*------------------------------------------------------------.
| yynewstate -- Push a new state, which is found in yystate.  |
`------------------------------------------------------------*/
 yynewstate:
  /* In all cases, when you get here, the value and location stacks
     have just been pushed.  So pushing a state here evens the stacks.  */
  yyssp++;

 yysetstate:
  *yyssp = yystate;

  if (yyss + yystacksize - 1 <= yyssp)
    {
      /* Get the current used size of the three stacks, in elements.  */
      YYSIZE_T yysize = yyssp - yyss + 1;

#ifdef yyoverflow
      {
        /* Give user a chance to reallocate the stack.  Use copies of
           these so that the &'s don't force the real ones into
           memory.  */
        YYSTYPE *yyvs1 = yyvs;
        yytype_int16 *yyss1 = yyss;

        /* Each stack pointer address is followed by the size of the
           data in use in that stack, in bytes.  This used to be a
           conditional around just the two extra args, but that might
           be undefined if yyoverflow is a macro.  */
        yyoverflow (YY_("memory exhausted"),
                    &yyss1, yysize * sizeof (*yyssp),
                    &yyvs1, yysize * sizeof (*yyvsp),
                    &yystacksize);

        yyss = yyss1;
        yyvs = yyvs1;
      }
#else /* no yyoverflow */
# ifndef YYSTACK_RELOCATE
      goto yyexhaustedlab;
# else
      /* Extend the stack our own way.  */
      if (YYMAXDEPTH <= yystacksize)
        goto yyexhaustedlab;
      yystacksize *= 2;
      if (YYMAXDEPTH < yystacksize)
        yystacksize = YYMAXDEPTH;

      {
        yytype_int16 *yyss1 = yyss;
        union yyalloc *yyptr =
          (union yyalloc *) YYSTACK_ALLOC (YYSTACK_BYTES (yystacksize));
        if (! yyptr)
          goto yyexhaustedlab;
        YYSTACK_RELOCATE (yyss_alloc, yyss);
        YYSTACK_RELOCATE (yyvs_alloc, yyvs);
#  undef YYSTACK_RELOCATE
        if (yyss1 != yyssa)
          YYSTACK_FREE (yyss1);
      }
# endif
#endif /* no yyoverflow */

      yyssp = yyss + yysize - 1;
      yyvsp = yyvs + yysize - 1;

      YYDPRINTF ((stderr, "Stack size increased to %lu\n",
                  (unsigned long int) yystacksize));

      if (yyss + yystacksize - 1 <= yyssp)
        YYABORT;
    }

  YYDPRINTF ((stderr, "Entering state %d\n", yystate));

  if (yystate == YYFINAL)
    YYACCEPT;

  goto yybackup;

/*-----------.
| yybackup.  |
`-----------*/
yybackup:

  /* Do appropriate processing given the current state.  Read a
     lookahead token if we need one and don't already have one.  */

  /* First try to decide what to do without reference to lookahead token.  */
  yyn = yypact[yystate];
  if (yypact_value_is_default (yyn))
    goto yydefault;

  /* Not known => get a lookahead token if don't already have one.  */

  /* YYCHAR is either YYEMPTY or YYEOF or a valid lookahead symbol.  */
  if (yychar == YYEMPTY)
    {
      YYDPRINTF ((stderr, "Reading a token: "));
      yychar = yylex ();
    }

  if (yychar <= YYEOF)
    {
      yychar = yytoken = YYEOF;
      YYDPRINTF ((stderr, "Now at end of input.\n"));
    }
  else
    {
      yytoken = YYTRANSLATE (yychar);
      YY_SYMBOL_PRINT ("Next token is", yytoken, &yylval, &yylloc);
    }

  /* If the proper action on seeing token YYTOKEN is to reduce or to
     detect an error, take that action.  */
  yyn += yytoken;
  if (yyn < 0 || YYLAST < yyn || yycheck[yyn] != yytoken)
    goto yydefault;
  yyn = yytable[yyn];
  if (yyn <= 0)
    {
      if (yytable_value_is_error (yyn))
        goto yyerrlab;
      yyn = -yyn;
      goto yyreduce;
    }

  /* Count tokens shifted since error; after three, turn off error
     status.  */
  if (yyerrstatus)
    yyerrstatus--;

  /* Shift the lookahead token.  */
  YY_SYMBOL_PRINT ("Shifting", yytoken, &yylval, &yylloc);

  /* Discard the shifted token.  */
  yychar = YYEMPTY;

  yystate = yyn;
  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  *++yyvsp = yylval;
  YY_IGNORE_MAYBE_UNINITIALIZED_END

  goto yynewstate;


/*-----------------------------------------------------------.
| yydefault -- do the default action for the current state.  |
`-----------------------------------------------------------*/
yydefault:
  yyn = yydefact[yystate];
  if (yyn == 0)
    goto yyerrlab;
  goto yyreduce;


/*-----------------------------.
| yyreduce -- Do a reduction.  |
`-----------------------------*/
yyreduce:
  /* yyn is the number of a rule to reduce with.  */
  yylen = yyr2[yyn];

  /* If YYLEN is nonzero, implement the default value of the action:
     '$$ = $1'.

     Otherwise, the following line sets YYVAL to garbage.
     This behavior is undocumented and Bison
     users should not rely upon it.  Assigning to YYVAL
     unconditionally makes the parser a bit smaller, and it avoids a
     GCC warning that YYVAL may be used uninitialized.  */
  yyval = yyvsp[1-yylen];


  YY_REDUCE_PRINT (yyn);
  switch (yyn)
    {
        case 12:
#line 195 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1646  */
    { library_jars->push_back(duplicate(yylval)); }
#line 1529 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
    break;

  case 13:
#line 199 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1646  */
    {keep_rule_start();}
#line 1535 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
    break;

  case 14:
#line 201 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1646  */
    {keep_rule_end();}
#line 1541 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
    break;

  case 15:
#line 204 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1646  */
    {keeprule->allow_deletion = false;
      keeprule->allow_cls_rename = true; keeprule->allow_member_rename = true;}
#line 1548 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
    break;

  case 16:
#line 206 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1646  */
    {keeprule->allow_deletion = true;
      keeprule->allow_cls_rename = false; keeprule->allow_member_rename = false;}
#line 1555 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
    break;

  case 17:
#line 208 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1646  */
    {keeprule->allow_deletion = false;
      keeprule->allow_cls_rename = true; keeprule->allow_member_rename = true;}
#line 1562 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
    break;

  case 18:
#line 210 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1646  */
    {keeprule->allow_deletion = true;
      keeprule->allow_cls_rename = true; keeprule->allow_member_rename = false;}
#line 1569 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
    break;

  case 19:
#line 212 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1646  */
    {keeprule->allow_deletion = false;
      keeprule->allow_cls_rename = false; keeprule->allow_member_rename = false;}
#line 1576 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
    break;

  case 20:
#line 214 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1646  */
    {keeprule->allow_deletion = true;
      keeprule->allow_cls_rename = true; keeprule->allow_member_rename = false;}
#line 1583 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
    break;

  case 23:
#line 222 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1646  */
    {WARN("'allowobfuscation' is not supported.\n"); }
#line 1589 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
    break;

  case 24:
#line 223 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1646  */
    {WARN("'allowoptimization' is not supported.\n");}
#line 1595 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
    break;

  case 25:
#line 224 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1646  */
    {keeprule->allow_deletion = true;}
#line 1601 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
    break;

  case 26:
#line 227 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1646  */
    {flags = 0;}
#line 1607 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
    break;

  case 27:
#line 227 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1646  */
    {if (keeprule) keeprule->flags = flags;}
#line 1613 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
    break;

  case 29:
#line 233 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1646  */
    {if (keeprule) keeprule->class_type = keeprules::ANY_CLASS_TYPE;}
#line 1619 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
    break;

  case 30:
#line 234 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1646  */
    {if (keeprule) keeprule->class_type = keeprules::CLASS;}
#line 1625 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
    break;

  case 31:
#line 235 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1646  */
    {if (keeprule) keeprule->class_type = keeprules::ENUMERATION;}
#line 1631 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
    break;

  case 32:
#line 236 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1646  */
    {if (keeprule) keeprule->class_type = keeprules::INTERFACE;}
#line 1637 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
    break;

  case 33:
#line 237 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1646  */
    {if (keeprule) keeprule->class_type = keeprules::ANNOTATION;}
#line 1643 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
    break;

  case 34:
#line 240 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1646  */
    {if (keeprule) keeprule->classname = duplicate(yylval);}
#line 1649 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
    break;

  case 35:
#line 243 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1646  */
    {if (keeprule) keeprule->extends = nullptr;}
#line 1655 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
    break;

  case 36:
#line 244 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1646  */
    {if (keeprule) keeprule->extends = duplicate(yylval);}
#line 1661 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
    break;

  case 37:
#line 245 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1646  */
    {if (keeprule) keeprule->extends = duplicate(yylval);}
#line 1667 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
    break;

  case 42:
#line 256 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1646  */
    {member_start();}
#line 1673 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
    break;

  case 43:
#line 262 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1646  */
    {member_end();}
#line 1679 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
    break;

  case 45:
#line 266 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1646  */
    {member_annotation = duplicate(yylval);}
#line 1685 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
    break;

  case 47:
#line 270 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1646  */
    {flags |= keeprules::PUBLIC;}
#line 1691 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
    break;

  case 48:
#line 271 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1646  */
    {flags |= keeprules::PRIVATE;}
#line 1697 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
    break;

  case 49:
#line 272 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1646  */
    {flags |= keeprules::PROTECTED;}
#line 1703 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
    break;

  case 54:
#line 283 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1646  */
    {flags |= keeprules::STATIC;}
#line 1709 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
    break;

  case 55:
#line 284 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1646  */
    {flags |= keeprules::FINAL;}
#line 1715 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
    break;

  case 56:
#line 285 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1646  */
    {flags |= keeprules::TRANSIENT;}
#line 1721 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
    break;

  case 57:
#line 286 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1646  */
    {flags |= keeprules::NATIVE;}
#line 1727 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
    break;

  case 61:
#line 292 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1646  */
    {member_type = "*"; member_name = "*";}
#line 1733 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
    break;

  case 62:
#line 293 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1646  */
    {member_type = "*";}
#line 1739 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
    break;

  case 63:
#line 293 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1646  */
    {member_name = duplicate(yylval);}
#line 1745 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
    break;

  case 64:
#line 294 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1646  */
    {member_type = duplicate(yylval);}
#line 1751 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
    break;

  case 65:
#line 294 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1646  */
    {member_name = "*";}
#line 1757 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
    break;

  case 66:
#line 295 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1646  */
    {member_type = duplicate(yylval);}
#line 1763 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
    break;

  case 67:
#line 295 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1646  */
    {member_name = duplicate(yylval);}
#line 1769 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
    break;

  case 69:
#line 300 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1646  */
    {member_args_start(); member_args_end();/* Method that takes no args */}
#line 1775 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
    break;

  case 71:
#line 301 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1646  */
    {member_args_start(); /* Method with args */}
#line 1781 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
    break;

  case 72:
#line 301 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1646  */
    {member_args_end();}
#line 1787 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
    break;

  case 76:
#line 308 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1646  */
    {if (method_filter) { method_filter->params.push_back(duplicate(yylval));}}
#line 1793 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
    break;


#line 1797 "/Users/drussi/fbsource/fbandroid/buck-out/gen/native/redex/config_generate_parser_cc/parser.cc" /* yacc.c:1646  */
      default: break;
    }
  /* User semantic actions sometimes alter yychar, and that requires
     that yytoken be updated with the new translation.  We take the
     approach of translating immediately before every use of yytoken.
     One alternative is translating here after every semantic action,
     but that translation would be missed if the semantic action invokes
     YYABORT, YYACCEPT, or YYERROR immediately after altering yychar or
     if it invokes YYBACKUP.  In the case of YYABORT or YYACCEPT, an
     incorrect destructor might then be invoked immediately.  In the
     case of YYERROR or YYBACKUP, subsequent parser actions might lead
     to an incorrect destructor call or verbose syntax error message
     before the lookahead is translated.  */
  YY_SYMBOL_PRINT ("-> $$ =", yyr1[yyn], &yyval, &yyloc);

  YYPOPSTACK (yylen);
  yylen = 0;
  YY_STACK_PRINT (yyss, yyssp);

  *++yyvsp = yyval;

  /* Now 'shift' the result of the reduction.  Determine what state
     that goes to, based on the state we popped back to and the rule
     number reduced by.  */

  yyn = yyr1[yyn];

  yystate = yypgoto[yyn - YYNTOKENS] + *yyssp;
  if (0 <= yystate && yystate <= YYLAST && yycheck[yystate] == *yyssp)
    yystate = yytable[yystate];
  else
    yystate = yydefgoto[yyn - YYNTOKENS];

  goto yynewstate;


/*--------------------------------------.
| yyerrlab -- here on detecting error.  |
`--------------------------------------*/
yyerrlab:
  /* Make sure we have latest lookahead translation.  See comments at
     user semantic actions for why this is necessary.  */
  yytoken = yychar == YYEMPTY ? YYEMPTY : YYTRANSLATE (yychar);

  /* If not already recovering from an error, report this error.  */
  if (!yyerrstatus)
    {
      ++yynerrs;
#if ! YYERROR_VERBOSE
      yyerror (YY_("syntax error"));
#else
# define YYSYNTAX_ERROR yysyntax_error (&yymsg_alloc, &yymsg, \
                                        yyssp, yytoken)
      {
        char const *yymsgp = YY_("syntax error");
        int yysyntax_error_status;
        yysyntax_error_status = YYSYNTAX_ERROR;
        if (yysyntax_error_status == 0)
          yymsgp = yymsg;
        else if (yysyntax_error_status == 1)
          {
            if (yymsg != yymsgbuf)
              YYSTACK_FREE (yymsg);
            yymsg = (char *) YYSTACK_ALLOC (yymsg_alloc);
            if (!yymsg)
              {
                yymsg = yymsgbuf;
                yymsg_alloc = sizeof yymsgbuf;
                yysyntax_error_status = 2;
              }
            else
              {
                yysyntax_error_status = YYSYNTAX_ERROR;
                yymsgp = yymsg;
              }
          }
        yyerror (yymsgp);
        if (yysyntax_error_status == 2)
          goto yyexhaustedlab;
      }
# undef YYSYNTAX_ERROR
#endif
    }



  if (yyerrstatus == 3)
    {
      /* If just tried and failed to reuse lookahead token after an
         error, discard it.  */

      if (yychar <= YYEOF)
        {
          /* Return failure if at end of input.  */
          if (yychar == YYEOF)
            YYABORT;
        }
      else
        {
          yydestruct ("Error: discarding",
                      yytoken, &yylval);
          yychar = YYEMPTY;
        }
    }

  /* Else will try to reuse lookahead token after shifting the error
     token.  */
  goto yyerrlab1;


/*---------------------------------------------------.
| yyerrorlab -- error raised explicitly by YYERROR.  |
`---------------------------------------------------*/
yyerrorlab:

  /* Pacify compilers like GCC when the user code never invokes
     YYERROR and the label yyerrorlab therefore never appears in user
     code.  */
  if (/*CONSTCOND*/ 0)
     goto yyerrorlab;

  /* Do not reclaim the symbols of the rule whose action triggered
     this YYERROR.  */
  YYPOPSTACK (yylen);
  yylen = 0;
  YY_STACK_PRINT (yyss, yyssp);
  yystate = *yyssp;
  goto yyerrlab1;


/*-------------------------------------------------------------.
| yyerrlab1 -- common code for both syntax error and YYERROR.  |
`-------------------------------------------------------------*/
yyerrlab1:
  yyerrstatus = 3;      /* Each real token shifted decrements this.  */

  for (;;)
    {
      yyn = yypact[yystate];
      if (!yypact_value_is_default (yyn))
        {
          yyn += YYTERROR;
          if (0 <= yyn && yyn <= YYLAST && yycheck[yyn] == YYTERROR)
            {
              yyn = yytable[yyn];
              if (0 < yyn)
                break;
            }
        }

      /* Pop the current state because it cannot handle the error token.  */
      if (yyssp == yyss)
        YYABORT;


      yydestruct ("Error: popping",
                  yystos[yystate], yyvsp);
      YYPOPSTACK (1);
      yystate = *yyssp;
      YY_STACK_PRINT (yyss, yyssp);
    }

  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  *++yyvsp = yylval;
  YY_IGNORE_MAYBE_UNINITIALIZED_END


  /* Shift the error token.  */
  YY_SYMBOL_PRINT ("Shifting", yystos[yyn], yyvsp, yylsp);

  yystate = yyn;
  goto yynewstate;


/*-------------------------------------.
| yyacceptlab -- YYACCEPT comes here.  |
`-------------------------------------*/
yyacceptlab:
  yyresult = 0;
  goto yyreturn;

/*-----------------------------------.
| yyabortlab -- YYABORT comes here.  |
`-----------------------------------*/
yyabortlab:
  yyresult = 1;
  goto yyreturn;

#if !defined yyoverflow || YYERROR_VERBOSE
/*-------------------------------------------------.
| yyexhaustedlab -- memory exhaustion comes here.  |
`-------------------------------------------------*/
yyexhaustedlab:
  yyerror (YY_("memory exhausted"));
  yyresult = 2;
  /* Fall through.  */
#endif

yyreturn:
  if (yychar != YYEMPTY)
    {
      /* Make sure we have latest lookahead translation.  See comments at
         user semantic actions for why this is necessary.  */
      yytoken = YYTRANSLATE (yychar);
      yydestruct ("Cleanup: discarding lookahead",
                  yytoken, &yylval);
    }
  /* Do not reclaim the symbols of the rule whose action triggered
     this YYABORT or YYACCEPT.  */
  YYPOPSTACK (yylen);
  YY_STACK_PRINT (yyss, yyssp);
  while (yyssp != yyss)
    {
      yydestruct ("Cleanup: popping",
                  yystos[*yyssp], yyvsp);
      YYPOPSTACK (1);
    }
#ifndef yyoverflow
  if (yyss != yyssa)
    YYSTACK_FREE (yyss);
#endif
#if YYERROR_VERBOSE
  if (yymsg != yymsgbuf)
    YYSTACK_FREE (yymsg);
#endif
  return yyresult;
}
#line 357 "/Users/drussi/fbsource/fbandroid/native/redex/configparser/config.y" /* yacc.c:1906  */


bool parse_proguard_file(const char * file, std::vector<KeepRule>* passed_rules,
                         std::vector<std::string>* passed_library_jars) {
    FILE *pgfile = fopen(file, "r");
    if (!pgfile) {
        std::cerr << "Couldn't open " << file << std::endl;
        return false;
    }
    yyin = pgfile;
    rules = passed_rules;
    library_jars = passed_library_jars;
    // parse through the input until there is no more:
    do {
        yyparse();
    } while (!feof(yyin));

    return true;
}

void yyerror(char const * msg) {
    printf("Parse error on line %d: %s\n", line_number, msg);
}
