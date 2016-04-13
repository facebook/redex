/* A Bison parser, made by GNU Bison 3.0.4.  */

/* Bison interface for Yacc-like parsers in C

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

#ifndef YY_YY_USERS_DRUSSI_FBSOURCE_FBANDROID_BUCK_OUT_GEN_NATIVE_REDEX_CONFIG_GENERATE_PARSER_HH_PARSER_HH_INCLUDED
# define YY_YY_USERS_DRUSSI_FBSOURCE_FBANDROID_BUCK_OUT_GEN_NATIVE_REDEX_CONFIG_GENERATE_PARSER_HH_PARSER_HH_INCLUDED
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

#endif /* !YY_YY_USERS_DRUSSI_FBSOURCE_FBANDROID_BUCK_OUT_GEN_NATIVE_REDEX_CONFIG_GENERATE_PARSER_HH_PARSER_HH_INCLUDED  */
