/* A Bison parser, made by GNU Bison 2.3.  */

/* Skeleton interface for Bison's Yacc-like parsers in C

   Copyright (C) 1984, 1989, 1990, 2000, 2001, 2002, 2003, 2004, 2005, 2006
   Free Software Foundation, Inc.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor,
   Boston, MA 02110-1301, USA.  */

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

/* Tokens.  */
#ifndef YYTOKENTYPE
# define YYTOKENTYPE
   /* Put the tokens into the symbol table, so that GDB and other debuggers
      know about them.  */
   enum yytokentype {
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
     T_FLATTENPACKAGEHIERARCHY = 286,
     T_INJARS = 287,
     T_KEEPATTRIBUTES = 288,
     T_KEEPPACKAGENAMES = 289,
     T_KEEPPARAMETERNAMES = 290,
     T_LIBRARYJARS = 291,
     T_MERGEINTERFACESAGGRESSIVELY = 292,
     T_OBFUSCATIONDICTIONARY = 293,
     T_OPTIMIZATIONPASSES = 294,
     T_OPTIMIZATIONS = 295,
     T_OUTJARS = 296,
     T_OVERLOADAGGRESSIVELY = 297,
     T_PACKAGEOBFUSCATIONDICTIONARY = 298,
     T_PRINTCONFIGURATION = 299,
     T_PRINTMAPPING = 300,
     T_PRINTSEEDS = 301,
     T_PRINTUSAGE = 302,
     T_RENAMESOURCEFILEATTRIBUTE = 303,
     T_REPACKAGECLASSES = 304,
     T_USEUNIQUECLASSMEMBERNAMES = 305,
     T_VERBOSE = 306,
     T_WHYAREYOUKEEPING = 307,
     T_CLASS = 308,
     T_ENUM = 309,
     T_INTERFACE = 310,
     T_AT_INTERFACE = 311,
     T_INIT = 312,
     T_IMPLEMENTS = 313,
     T_EXTENDS = 314,
     T_PUBLIC = 315,
     T_PRIVATE = 316,
     T_PROTECTED = 317,
     T_STATIC = 318,
     T_FINAL = 319,
     T_TRANSIENT = 320,
     T_NATIVE = 321,
     T_METHODS = 322,
     T_FIELDS = 323,
     T_ANY_MEMBER = 324,
     T_PATTERN = 325,
     T_MEMBERS_BEGIN = 326,
     T_MEMBERS_END = 327,
     T_ARGS_BEGIN = 328,
     T_ARGS_END = 329
   };
#endif
/* Tokens.  */
#define T_NEWLINE 258
#define T_SEMICOLON 259
#define T_COMMA 260
#define T_NOT 261
#define T_AT 262
#define T_COMMENT 263
#define T_KEEP 264
#define T_KEEPNAMES 265
#define T_KEEPCLASSMEMBERS 266
#define T_KEEPCLASSMEMBERNAMES 267
#define T_KEEPCLASSESWITHMEMBERS 268
#define T_KEEPCLASSESWITHMEMBERNAMES 269
#define T_ALLOWOBFUSCATION 270
#define T_ALLOWOPTIMIZATION 271
#define T_ALLOWSHRINKING 272
#define T_ADAPTCLASSSTRINGS 273
#define T_ADAPTRESOURCEFILECONTENTS 274
#define T_ADAPTRESOURCEFILENAMES 275
#define T_ALLOWACCESSMODIFICATION 276
#define T_APPLYMAPPING 277
#define T_ASSUMENOSIDEEFFECTS 278
#define T_CLASSOBFUSCATIONDICTIONARY 279
#define T_DONTOBFUSCATE 280
#define T_DONTOPTIMIZE 281
#define T_DONTPREVERIFY 282
#define T_DONTSHRINK 283
#define T_DONTWARN 284
#define T_DONTUSEMIXEDCASECLASSNAMES 285
#define T_FLATTENPACKAGEHIERARCHY 286
#define T_INJARS 287
#define T_KEEPATTRIBUTES 288
#define T_KEEPPACKAGENAMES 289
#define T_KEEPPARAMETERNAMES 290
#define T_LIBRARYJARS 291
#define T_MERGEINTERFACESAGGRESSIVELY 292
#define T_OBFUSCATIONDICTIONARY 293
#define T_OPTIMIZATIONPASSES 294
#define T_OPTIMIZATIONS 295
#define T_OUTJARS 296
#define T_OVERLOADAGGRESSIVELY 297
#define T_PACKAGEOBFUSCATIONDICTIONARY 298
#define T_PRINTCONFIGURATION 299
#define T_PRINTMAPPING 300
#define T_PRINTSEEDS 301
#define T_PRINTUSAGE 302
#define T_RENAMESOURCEFILEATTRIBUTE 303
#define T_REPACKAGECLASSES 304
#define T_USEUNIQUECLASSMEMBERNAMES 305
#define T_VERBOSE 306
#define T_WHYAREYOUKEEPING 307
#define T_CLASS 308
#define T_ENUM 309
#define T_INTERFACE 310
#define T_AT_INTERFACE 311
#define T_INIT 312
#define T_IMPLEMENTS 313
#define T_EXTENDS 314
#define T_PUBLIC 315
#define T_PRIVATE 316
#define T_PROTECTED 317
#define T_STATIC 318
#define T_FINAL 319
#define T_TRANSIENT 320
#define T_NATIVE 321
#define T_METHODS 322
#define T_FIELDS 323
#define T_ANY_MEMBER 324
#define T_PATTERN 325
#define T_MEMBERS_BEGIN 326
#define T_MEMBERS_END 327
#define T_ARGS_BEGIN 328
#define T_ARGS_END 329




#if ! defined YYSTYPE && ! defined YYSTYPE_IS_DECLARED
typedef int YYSTYPE;
# define yystype YYSTYPE /* obsolescent; will be withdrawn */
# define YYSTYPE_IS_DECLARED 1
# define YYSTYPE_IS_TRIVIAL 1
#endif

extern YYSTYPE yylval;

