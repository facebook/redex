%{
/**
 * Copyright (c) 2016-present, Facebook, Inc.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree. An additional grant
 * of patent rights can be found in the PATENTS file in the same directory.
 */

#pragma clang diagnostic ignored "-Wdeprecated-register"
#pragma clang diagnostic ignored "-Wnull-conversion"
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunneeded-internal-declaration"

#define YYSTYPE char*

#include <iostream>
#include <cstdio>
#include "parser.hh"

#define RED "\033[0;31m"
#define NO_COLOR "\033[0m"
#define WHENDEBUG(x) fprintf(stderr, RED x " %d  %s" NO_COLOR "  \n", line_number, yytext)
#define DEBUG(x)
#define YY_DECL extern "C" int yylex()

int line_number = 1;

%}

%option noyywrap

T_WS [ \t]+
T_NEWLINE \n
T_COMMA ","
T_SEMICOLON ";"
T_AT "@"
T_QUOTE "'"
T_CLASS "class"
T_INTERFACE "interface"
T_AT_INTERFACE "@interface"
T_ENUM "enum"
T_IMPLEMENTS "implements"
T_EXTENDS "extends"
T_PUBLIC "public"
T_PRIVATE "private"
T_PROTECTED "protected"
T_NOT "!"
T_FIELDS "<fields>"
T_METHODS "<methods>"
T_INIT "<init>"

T_KEEPCLASSESWITHMEMBERNAMES "-keepclasseswithmembernames"
T_KEEPCLASSESWITHMEMBERS "-keepclasseswithmembers"
T_KEEPCLASSMEMBERNAMES "-keepclassmembernames"
T_KEEPCLASSMEMBERS "-keepclassmembers"
T_KEEPNAMES "-keepnames"
T_KEEP "-keep"

T_ALLOWOBFUSCATION "allowobfuscation"
T_ALLOWOPTIMIZATION "allowoptimization"
T_ALLOWSHRINKING "allowshrinking"

T_ADAPTCLASSSTRINGS "-adaptclassstrings"
T_ADAPTRESOURCEFILECONTENTS "-adaptresourcefilecontents"
T_ADAPTRESOURCEFILENAMES "-adaptresourcefilenames"
T_ALLOWACCESSMODIFICATION "-allowaccessmodification"
T_APPLYMAPPING "-applymapping"
T_ASSUMENOSIDEEFFECTS "-assumenosideeffects"
T_CLASSOBFUSCATIONDICTIONARY "-classobfuscationdictionary"
T_DONTNOTE "-dontnote"
T_DONTOBFUSCATE "-dontobfuscate"
T_DONTOPTIMIZE "-dontoptimize"
T_DONTPREVERIFY "-dontpreverify"
T_DONTSHRINK "-dontshrink"
T_DONTWARN "-dontwarn"
T_DONTUSEMIXEDCASECLASSNAMES "-dontusemixedcaseclassnames"
T_DONTSKIPNONPUBLICLIBRARYCLASSES "-dontskipnonpubliclibraryclasses"
T_FLATTENPACKAGEHIERARCHY "-flattenpackagehierarchy"
T_INCLUDE "-include"
T_INJARS "-injars"
T_OUTJARS "-outjars"
T_KEEPATTRIBUTES "-keepattributes"
T_KEEPPACKAGENAMES "-keeppackagenames"
T_KEEPPARAMETERNAMES "-keepparameternames"
T_LIBRARYJARS "-libraryjars"
T_MERGEINTERFACESAGGRESSIVELY "-mergeinterfacesaggressively"
T_OBFUSCATIONDICTIONARY "-obfuscationdictionary"
T_OPTIMIZATIONPASSES "-optimizationpasses"
T_OPTIMIZATIONS "-optimizations"
T_OVERLOADAGGRESSIVELY "-overloadaggressively"
T_PACKAGEOBFUSCATIONDICTIONARY "-packageobfuscationdictionary"
T_PRINTCONFIGURATION "-printconfiguration"
T_PRINTMAPPING "-printmapping"
T_PRINTSEEDS "-printseeds"
T_PRINTUSAGE "-printusage"
T_RENAMESOURCEFILEATTRIBUTE "-renamesourcefileattribute"
T_REPACKAGECLASSES "-repackageclasses"
T_USEUNIQUECLASSMEMBERNAMES "-useuniqueclassmembernames"
T_VERBOSE "-verbose"
T_WHYAREYOUKEEPING "-whyareyoukeeping"

T_STATIC "static"
T_FINAL "final"
T_TRANSIENT "transient"
T_NATIVE "native"

T_MEMBERS_BEGIN "{"
T_MEMBERS_END "}"
T_ARGS_BEGIN "("
T_ARGS_END ")"

T_COMMENT #[^\n]*

/* For the moment we conflate tokens for patterns and filenames. TODO: fix. */
T_PATTERN [A-Za-z*[.0-9/$_%!]+[A-Za-z*\[\].0-9/$_#%!-]*

%%


{T_WS}          /* Ignore whitespace */;
{T_NEWLINE}     {line_number++;}

{T_AT}                           {DEBUG("at               ");  return T_AT;}
{T_COMMA}                        {DEBUG("comma            ");  return T_COMMA;}
{T_SEMICOLON}                    {DEBUG("semicolon        ");  return T_SEMICOLON;}
{T_NOT}                          {DEBUG("not              ");  return T_NOT;}
{T_QUOTE}                        {DEBUG("quote            ");  return T_QUOTE;}

\*/;                             {DEBUG("any_member       ");  /* Can't use {ANY_MEMBER} at start of line due to bug in flex */ return T_ANY_MEMBER;}

{T_COMMENT}                      {DEBUG("comment          ");  return T_COMMENT;}

{T_ALLOWOBFUSCATION}             {DEBUG("allowobfuscation ");  return T_ALLOWOBFUSCATION;}
{T_ALLOWOPTIMIZATION}            {DEBUG("allowoptimization");  return T_ALLOWOPTIMIZATION;}
{T_ALLOWSHRINKING}               {DEBUG("allowshrinking   ");  return T_ALLOWSHRINKING;}

{T_KEEPCLASSESWITHMEMBERS}       {DEBUG("keep cls w/      ");  return T_KEEPCLASSESWITHMEMBERS;}
{T_KEEPCLASSESWITHMEMBERNAMES}   {DEBUG("keep cls w/names ");  return T_KEEPCLASSESWITHMEMBERNAMES;}
{T_KEEPCLASSMEMBERNAMES}         {DEBUG("keepmembernames  ");  return T_KEEPCLASSMEMBERNAMES;}
{T_KEEPCLASSMEMBERS}             {DEBUG("keepmembers      ");  return T_KEEPCLASSMEMBERS;}
{T_KEEPNAMES}                    {DEBUG("keepnames        ");  return T_KEEPNAMES;}
{T_KEEP}                         {DEBUG("keep             ");  return T_KEEP;}

{T_STATIC}                       {DEBUG("static           ");  return T_STATIC;}
{T_FINAL}                        {DEBUG("final            ");  return T_FINAL;}
{T_TRANSIENT}                    {DEBUG("transient        ");  return T_TRANSIENT;}
{T_NATIVE}                       {DEBUG("native           ");  return T_NATIVE;}

{T_CLASS}                        {DEBUG("class            ");  return T_CLASS;}
{T_ENUM}                         {DEBUG("enum             ");  return T_ENUM;}
{T_INTERFACE}                    {DEBUG("interface        ");  return T_INTERFACE;}
{T_AT_INTERFACE}                 {DEBUG("@interface       ");  return T_AT_INTERFACE;}

{T_EXTENDS}                      {DEBUG("extends          ");  return T_EXTENDS;}
{T_IMPLEMENTS}                   {DEBUG("implements       ");  return T_IMPLEMENTS;}

{T_MEMBERS_BEGIN}                {DEBUG("{                ");  return T_MEMBERS_BEGIN;}
{T_MEMBERS_END}                  {DEBUG("}                ");  return T_MEMBERS_END;}

{T_ARGS_BEGIN}                   {DEBUG("(                ");  return T_ARGS_BEGIN;}
{T_ARGS_END}                     {DEBUG(")                ");  return T_ARGS_END;}

{T_FIELDS}                       {DEBUG("fields           ");  return T_FIELDS;}
{T_METHODS}                      {DEBUG("methods          ");  return T_METHODS;}

{T_INIT}                         {DEBUG("init             ");  return T_INIT;}

{T_PUBLIC}                       {DEBUG("public           ");  return T_PUBLIC;}
{T_PRIVATE}                      {DEBUG("private          ");  return T_PRIVATE;}
{T_PROTECTED}                    {DEBUG("protected        ");  return T_PROTECTED;}

{T_LIBRARYJARS}                  {DEBUG("libraryjars       ");  return T_LIBRARYJARS;}

{T_INJARS}                       {DEBUG("injars            ");  return T_INJARS;}
{T_OUTJARS}                      {DEBUG("outjars           ");  return T_OUTJARS;}

{T_ADAPTCLASSSTRINGS}            {DEBUG("[ignored rule]   "); return T_ADAPTCLASSSTRINGS;}
{T_ADAPTRESOURCEFILECONTENTS}    {DEBUG("[ignored rule]   "); return T_ADAPTRESOURCEFILECONTENTS;}
{T_ADAPTRESOURCEFILENAMES}       {DEBUG("[ignored rule]   "); return T_ADAPTRESOURCEFILENAMES;}
{T_ALLOWACCESSMODIFICATION}      {DEBUG("[ignored rule]   "); return T_ALLOWACCESSMODIFICATION;}
{T_APPLYMAPPING}                 {DEBUG("[ignored rule]   "); return T_APPLYMAPPING;}
{T_ASSUMENOSIDEEFFECTS}          {DEBUG("[ignored rule]   "); return T_ASSUMENOSIDEEFFECTS;}
{T_CLASSOBFUSCATIONDICTIONARY}   {DEBUG("[ignored rule]   "); return T_CLASSOBFUSCATIONDICTIONARY;}
{T_DONTNOTE}                     {DEBUG("[ignored rule]   "); return T_DONTNOTE;}
{T_DONTOBFUSCATE}                {DEBUG("[ignored rule]   "); return T_DONTOBFUSCATE;}
{T_DONTOPTIMIZE}                 {DEBUG("[ignored rule]   "); return T_DONTOPTIMIZE;}
{T_DONTPREVERIFY}                {DEBUG("[ignored rule]   "); return T_DONTPREVERIFY;}
{T_DONTSHRINK}                   {DEBUG("[ignored rule]   "); return T_DONTSHRINK;}
{T_DONTWARN}                     {DEBUG("[ignored rule]   "); return T_DONTWARN;}
{T_DONTUSEMIXEDCASECLASSNAMES}   {DEBUG("[ignored rule]   "); return T_DONTUSEMIXEDCASECLASSNAMES;}
{T_DONTSKIPNONPUBLICLIBRARYCLASSES} {DEBUG("[ignored rule]   "); return T_DONTSKIPNONPUBLICLIBRARYCLASSES;}
{T_FLATTENPACKAGEHIERARCHY}      {DEBUG("[ignored rule]   "); return T_FLATTENPACKAGEHIERARCHY;}
{T_INCLUDE}                      {DEBUG("[ignored rule]   "); return T_INCLUDE;}
{T_KEEPATTRIBUTES}               {DEBUG("[ignored rule]   "); return T_KEEPATTRIBUTES;}
{T_KEEPPACKAGENAMES}             {DEBUG("[ignored rule]   "); return T_KEEPPACKAGENAMES;}
{T_KEEPPARAMETERNAMES}           {DEBUG("[ignored rule]   "); return T_KEEPPARAMETERNAMES;}
{T_MERGEINTERFACESAGGRESSIVELY}  {DEBUG("[ignored rule]   "); return T_MERGEINTERFACESAGGRESSIVELY;}
{T_OBFUSCATIONDICTIONARY}        {DEBUG("[ignored rule]   "); return T_OBFUSCATIONDICTIONARY;}
{T_OPTIMIZATIONPASSES}           {DEBUG("[ignored rule]   "); return T_OPTIMIZATIONPASSES;}
{T_OPTIMIZATIONS}                {DEBUG("[ignored rule]   "); return T_OPTIMIZATIONS;}
{T_OVERLOADAGGRESSIVELY}         {DEBUG("[ignored rule]   "); return T_OVERLOADAGGRESSIVELY;}
{T_PACKAGEOBFUSCATIONDICTIONARY} {DEBUG("[ignored rule]   "); return T_PACKAGEOBFUSCATIONDICTIONARY;}
{T_PRINTCONFIGURATION}           {DEBUG("[ignored rule]   "); return T_PRINTCONFIGURATION;}
{T_PRINTMAPPING}                 {DEBUG("[ignored rule]   "); return T_PRINTMAPPING;}
{T_PRINTSEEDS}                   {DEBUG("[ignored rule]   "); return T_PRINTSEEDS;}
{T_PRINTUSAGE}                   {DEBUG("[ignored rule]   "); return T_PRINTUSAGE;}
{T_RENAMESOURCEFILEATTRIBUTE}    {DEBUG("[ignored rule]   "); return T_RENAMESOURCEFILEATTRIBUTE;}
{T_REPACKAGECLASSES}             {DEBUG("[ignored rule]   "); return T_REPACKAGECLASSES;}
{T_USEUNIQUECLASSMEMBERNAMES}    {DEBUG("[ignored rule]   "); return T_USEUNIQUECLASSMEMBERNAMES;}
{T_VERBOSE}                      {DEBUG("[ignored rule]   "); return T_VERBOSE;}
{T_WHYAREYOUKEEPING}             {DEBUG("[ignored rule]   "); return T_WHYAREYOUKEEPING;}

{T_PATTERN}                      {DEBUG("pattern          "); yylval = yytext; return T_PATTERN;}


. {
    fprintf(stderr, "Unexpected token %s on line %d\n", yytext, line_number);
    exit(1);
}

%%
